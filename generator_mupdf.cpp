/*
    Okular backend for PDF documents based on MuPDF

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "generator_mupdf.h"

#include <core/action.h>
#include <core/area.h>
#include <core/document.h>
#include <core/page.h>
#include <core/textpage.h>

#include <KPluginFactory>

#include <QDateTime>
#include <QDomElement>
#include <QFile>
#include <QLocale>
#include <QLoggingCategory>
#include <QMutexLocker>
#include <QUrl>

Q_LOGGING_CATEGORY(OkularMuPdfDebug, "org.kde.okular.generators.mupdf", QtWarningMsg)

K_PLUGIN_CLASS_WITH_JSON(MuPdfGenerator, "libokularGenerator_mupdf.json")

// ---------------------------------------------------------------------------
// Small helpers around the MuPDF C API.
//
// MuPDF reports errors through fz_try/fz_catch, which is built on setjmp and
// longjmp. That is only safe if no C++ object with a destructor lives between
// fz_try and the throw point, so every helper below only uses plain old data
// and returns a status code.
// ---------------------------------------------------------------------------

static void warningCallback(void *, const char *message)
{
    qCDebug(OkularMuPdfDebug) << "MuPDF warning:" << message;
}

static void errorCallback(void *, const char *message)
{
    qCWarning(OkularMuPdfDebug) << "MuPDF error:" << message;
}

namespace
{
constexpr size_t StoreSize = size_t(512) << 20; // cache of decoded images/fonts/glyphs

struct PageInfo {
    fz_rect bounds;
};

fz_document *openDocument(fz_context *ctx, const char *path)
{
    fz_document *doc = nullptr;
    fz_try(ctx)
    {
        doc = fz_open_document(ctx, path);
    }
    fz_catch(ctx)
    {
        doc = nullptr;
    }
    return doc;
}

fz_document *openDocumentFromMemory(fz_context *ctx, const unsigned char *data, size_t size)
{
    fz_document *doc = nullptr;
    fz_buffer *buf = nullptr;
    fz_stream *stm = nullptr;
    fz_var(buf);
    fz_var(stm);
    fz_try(ctx)
    {
        buf = fz_new_buffer_from_copied_data(ctx, data, size);
        stm = fz_open_buffer(ctx, buf);
        doc = fz_open_document_with_stream(ctx, "application/pdf", stm);
    }
    fz_always(ctx)
    {
        fz_drop_stream(ctx, stm);
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx)
    {
        doc = nullptr;
    }
    return doc;
}

int countPages(fz_context *ctx, fz_document *doc)
{
    int n = -1;
    fz_try(ctx)
    {
        n = fz_count_pages(ctx, doc);
    }
    fz_catch(ctx)
    {
        n = -1;
    }
    return n;
}

bool pageBounds(fz_context *ctx, fz_document *doc, int number, fz_rect *out)
{
    fz_page *page = nullptr;
    bool ok = true;
    fz_var(page);
    fz_try(ctx)
    {
        page = fz_load_page(ctx, doc, number);
        *out = fz_bound_page(ctx, page);
    }
    fz_always(ctx)
    {
        fz_drop_page(ctx, page);
    }
    fz_catch(ctx)
    {
        ok = false;
    }
    return ok;
}

bool renderPage(fz_context *ctx, fz_page *page, fz_matrix ctm, QImage &image)
{
    fz_pixmap *pix = nullptr;
    fz_device *dev = nullptr;
    bool ok = true;
    fz_var(pix);
    fz_var(dev);
    fz_try(ctx)
    {
        // Render straight into the QImage buffer: QImage::Format_RGB32 is
        // B,G,R,X in memory on little endian machines, which is exactly BGR+alpha.
        pix = fz_new_pixmap_with_data(ctx, fz_device_bgr(ctx), image.width(), image.height(), nullptr, 1, int(image.bytesPerLine()), image.bits());
        fz_clear_pixmap_with_value(ctx, pix, 0xff);
        dev = fz_new_draw_device(ctx, fz_identity, pix);
        fz_run_page(ctx, page, dev, ctm, nullptr);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx)
    {
        fz_drop_device(ctx, dev);
        fz_drop_pixmap(ctx, pix);
    }
    fz_catch(ctx)
    {
        ok = false;
    }
    return ok;
}

struct RawLink {
    fz_rect rect;
    QByteArray uri;
};

// Returns the page's links (in page space) and resolves the destination of
// internal links. All MuPDF types are plain so a QList of PODs-with-QByteArray
// is only touched outside of fz_try.
struct ResolvedLink {
    fz_rect rect;
    bool external = false;
    QByteArray uri;
    int page = -1;
    bool hasPos = false;
    float x = 0;
    float y = 0;
};

bool resolveLinkDest(fz_context *ctx, fz_document *doc, const char *uri, int *pageNumber, bool *hasPos, float *x, float *y)
{
    bool ok = true;
    fz_try(ctx)
    {
        const fz_link_dest dest = fz_resolve_link_dest(ctx, doc, uri);
        *pageNumber = fz_page_number_from_location(ctx, doc, dest.loc);
        // Only "XYZ" destinations really carry a position; for everything
        // else we go to the top of the page.
        *hasPos = dest.type == FZ_LINK_DEST_XYZ || dest.type == FZ_LINK_DEST_FIT_H || dest.type == FZ_LINK_DEST_FIT_BH;
        *x = dest.x;
        *y = dest.y;
    }
    fz_catch(ctx)
    {
        ok = false;
    }
    return ok;
}

} // namespace

// ---------------------------------------------------------------------------

MuPdfGenerator::MuPdfGenerator(QObject *parent, const QVariantList &args)
    : Okular::Generator(parent, args)
{
    setFeature(TextExtraction);
    setFeature(Threaded);
}

MuPdfGenerator::~MuPdfGenerator()
{
    // The document is dropped in doCloseDocument(), but be safe.
    if (m_ctx) {
        QMutexLocker lock(&m_mutex);
        closeInternal();
        fz_drop_context(m_ctx);
        m_ctx = nullptr;
    }
}

bool MuPdfGenerator::ensureContext()
{
    if (m_ctx) {
        return true;
    }
    m_ctx = fz_new_context(nullptr, nullptr, StoreSize);
    if (!m_ctx) {
        qCWarning(OkularMuPdfDebug) << "Could not create MuPDF context";
        return false;
    }
    fz_set_warning_callback(m_ctx, warningCallback, nullptr);
    fz_set_error_callback(m_ctx, errorCallback, nullptr);

    bool ok = true;
    fz_try(m_ctx)
    {
        fz_register_document_handlers(m_ctx);
    }
    fz_catch(m_ctx)
    {
        ok = false;
    }
    if (!ok) {
        fz_drop_context(m_ctx);
        m_ctx = nullptr;
    }
    return ok;
}

void MuPdfGenerator::closeInternal()
{
    delete m_synopsis;
    m_synopsis = nullptr;
    if (m_ctx && m_doc) {
        fz_drop_document(m_ctx, m_doc);
    }
    m_doc = nullptr;
    m_pageBounds.clear();
    m_linksGenerated.clear();
}

Okular::Document::OpenResult MuPdfGenerator::finishOpen(QList<Okular::Page *> &pagesVector, const QString &password)
{
    if (!m_doc) {
        return Okular::Document::OpenError;
    }

    bool needsPassword = false;
    fz_try(m_ctx)
    {
        needsPassword = fz_needs_password(m_ctx, m_doc);
    }
    fz_catch(m_ctx)
    {
        closeInternal();
        return Okular::Document::OpenError;
    }

    if (needsPassword) {
        bool authenticated = false;
        if (!password.isEmpty()) {
            const QByteArray pw = password.toUtf8();
            fz_try(m_ctx)
            {
                authenticated = fz_authenticate_password(m_ctx, m_doc, pw.constData());
            }
            fz_catch(m_ctx)
            {
                authenticated = false;
            }
        }
        if (!authenticated) {
            // Okular calls loadDocumentWithPassword() again with the password.
            closeInternal();
            return Okular::Document::OpenNeedsPassword;
        }
    }

    const int count = countPages(m_ctx, m_doc);
    if (count < 0) {
        closeInternal();
        return Okular::Document::OpenError;
    }

    QSizeF dpiSize = dpi();
    if (dpiSize.width() <= 0 || dpiSize.height() <= 0) {
        dpiSize = QSizeF(72, 72);
    }
    m_pageBounds.resize(count);
    m_linksGenerated.assign(count, false);
    pagesVector.resize(count);
    for (int i = 0; i < count; ++i) {
        fz_rect r = {0, 0, 612, 792}; // US Letter fallback for broken pages
        if (!pageBounds(m_ctx, m_doc, i, &r) || r.x1 <= r.x0 || r.y1 <= r.y0) {
            r = {0, 0, 612, 792};
        }
        m_pageBounds[i] = r;
        // fz_bound_page() already takes the /Rotate entry into account. Like the
        // Poppler backend we report sizes in screen pixels (pagesSizeMetric()).
        pagesVector[i] = new Okular::Page(i, (r.x1 - r.x0) / 72.0 * dpiSize.width(), (r.y1 - r.y0) / 72.0 * dpiSize.height(), Okular::Rotation0);
    }
    return Okular::Document::OpenSuccess;
}

Okular::Document::OpenResult MuPdfGenerator::loadDocumentWithPassword(const QString &fileName, QList<Okular::Page *> &pagesVector, const QString &password)
{
    QMutexLocker lock(&m_mutex);
    closeInternal();
    if (!ensureContext()) {
        return Okular::Document::OpenError;
    }
    m_doc = openDocument(m_ctx, QFile::encodeName(fileName).constData());
    return finishOpen(pagesVector, password);
}

Okular::Document::OpenResult MuPdfGenerator::loadDocumentFromDataWithPassword(const QByteArray &fileData, QList<Okular::Page *> &pagesVector, const QString &password)
{
    QMutexLocker lock(&m_mutex);
    closeInternal();
    if (!ensureContext()) {
        return Okular::Document::OpenError;
    }
    m_doc = openDocumentFromMemory(m_ctx, reinterpret_cast<const unsigned char *>(fileData.constData()), size_t(fileData.size()));
    return finishOpen(pagesVector, password);
}

bool MuPdfGenerator::doCloseDocument()
{
    QMutexLocker lock(&m_mutex);
    closeInternal();
    return true;
}

// ---------------------------------------------------------------------------
// Links

static Okular::NormalizedRect normalizedRect(const fz_rect &r, const fz_rect &bounds)
{
    const double w = bounds.x1 - bounds.x0;
    const double h = bounds.y1 - bounds.y0;
    return Okular::NormalizedRect((r.x0 - bounds.x0) / w, (r.y0 - bounds.y0) / h, (r.x1 - bounds.x0) / w, (r.y1 - bounds.y0) / h);
}

static Okular::DocumentViewport viewportFor(int page, bool hasPos, float x, float y, const fz_rect &targetBounds)
{
    Okular::DocumentViewport vp(page);
    if (hasPos) {
        const double w = targetBounds.x1 - targetBounds.x0;
        const double h = targetBounds.y1 - targetBounds.y0;
        vp.rePos.enabled = true;
        vp.rePos.normalizedX = qBound(0.0, (x - targetBounds.x0) / w, 1.0);
        vp.rePos.normalizedY = qBound(0.0, (y - targetBounds.y0) / h, 1.0);
        vp.rePos.pos = Okular::DocumentViewport::TopLeft;
    }
    return vp;
}

// Called with m_mutex held.
QList<Okular::ObjectRect *> MuPdfGenerator::generateLinks(fz_page *page, int pageNumber)
{
    QList<Okular::ObjectRect *> result;
    const fz_rect bounds = m_pageBounds.at(pageNumber);

    // 1. collect the raw data (plain C, guarded)
    QList<ResolvedLink> links;
    fz_link *head = nullptr;
    fz_try(m_ctx)
    {
        head = fz_load_links(m_ctx, page);
    }
    fz_catch(m_ctx)
    {
        head = nullptr;
    }
    for (fz_link *l = head; l; l = l->next) {
        if (!l->uri) {
            continue;
        }
        ResolvedLink rl;
        rl.rect = l->rect;
        rl.uri = l->uri;
        rl.external = fz_is_external_link(m_ctx, l->uri);
        if (!rl.external) {
            if (!resolveLinkDest(m_ctx, m_doc, l->uri, &rl.page, &rl.hasPos, &rl.x, &rl.y)) {
                continue;
            }
            if (rl.page < 0 || rl.page >= m_pageBounds.size()) {
                continue;
            }
        }
        links.append(rl);
    }
    if (head) {
        fz_drop_link(m_ctx, head);
    }

    // 2. translate to Okular objects
    for (const ResolvedLink &rl : std::as_const(links)) {
        Okular::Action *action = nullptr;
        if (rl.external) {
            action = new Okular::BrowseAction(QUrl(QString::fromUtf8(rl.uri)));
        } else {
            action = new Okular::GotoAction(QString(), viewportFor(rl.page, rl.hasPos, rl.x, rl.y, m_pageBounds.at(rl.page)));
        }
        result.append(new Okular::ObjectRect(normalizedRect(rl.rect, bounds), false, Okular::ObjectRect::Action, action));
    }
    return result;
}

// ---------------------------------------------------------------------------
// Rendering

QImage MuPdfGenerator::image(Okular::PixmapRequest *request)
{
    Okular::Page *okularPage = request->page();
    const int number = okularPage->number();

    QMutexLocker lock(&m_mutex);

    if (!m_doc || number < 0 || number >= m_pageBounds.size()) {
        return QImage();
    }
    if (request->shouldAbortRender()) {
        return QImage();
    }

    const fz_rect bounds = m_pageBounds.at(number);
    const float pageW = bounds.x1 - bounds.x0;
    const float pageH = bounds.y1 - bounds.y0;

    // Okular hands us the size of the *unrotated* page; it rotates the
    // resulting pixmap itself.
    const float sx = request->width() / pageW;
    const float sy = request->height() / pageH;

    QRect target(0, 0, request->width(), request->height());
    if (request->isTile()) {
        target = request->normalizedRect().geometry(request->width(), request->height());
    }
    if (target.isEmpty()) {
        return QImage();
    }

    fz_matrix ctm = fz_translate(-bounds.x0, -bounds.y0);
    ctm = fz_concat(ctm, fz_scale(sx, sy));
    ctm = fz_concat(ctm, fz_translate(-target.x(), -target.y()));

    QImage img(target.size(), QImage::Format_RGB32);
    if (img.isNull()) {
        return QImage();
    }

    fz_page *page = nullptr;
    bool ok = true;
    fz_try(m_ctx)
    {
        page = fz_load_page(m_ctx, m_doc, number);
    }
    fz_catch(m_ctx)
    {
        ok = false;
    }
    if (!ok) {
        img.fill(Qt::white);
        return img;
    }

    if (!renderPage(m_ctx, page, ctm, img)) {
        img.fill(Qt::white);
    }

    if (!m_linksGenerated[number]) {
        okularPage->setObjectRects(generateLinks(page, number));
        m_linksGenerated[number] = true;
    }

    fz_drop_page(m_ctx, page);
    return img;
}

// ---------------------------------------------------------------------------
// Text extraction

namespace
{
struct TextChar {
    QString text;
    fz_rect box;
};

struct TextWord {
    QList<TextChar> chars;
    fz_rect box = fz_empty_rect;
    bool endOfLine = false;
};

// Depth first walk over the (possibly nested) stext block tree. Mirrors what
// Okular's Poppler backend hands to TextPage: one entity per glyph, an
// explicit space entity between the words of a line and a trailing "\n" on
// the last glyph of each line.
void collectWords(fz_stext_block *first, QList<TextWord> &out)
{
    for (fz_stext_block *b = first; b; b = b->next) {
        if (b->type == FZ_STEXT_BLOCK_STRUCT) {
            if (b->u.s.down) {
                collectWords(b->u.s.down->first_block, out);
            }
            continue;
        }
        if (b->type != FZ_STEXT_BLOCK_TEXT) {
            continue;
        }
        for (fz_stext_line *line = b->u.t.first_line; line; line = line->next) {
            const qsizetype lineStart = out.size();
            TextWord word;
            auto flush = [&]() {
                if (!word.chars.isEmpty()) {
                    // Okular inserts a space between two glyphs whenever their
                    // boxes do not touch exactly, so make them contiguous
                    // (kerning produces small gaps and overlaps).
                    for (qsizetype k = 1; k < word.chars.size(); ++k) {
                        fz_rect &prev = word.chars[k - 1].box;
                        fz_rect &cur = word.chars[k].box;
                        const float mid = (prev.x1 + cur.x0) / 2;
                        prev.x1 = mid;
                        cur.x0 = mid;
                    }
                    out.append(word);
                }
                word = TextWord();
            };
            for (fz_stext_char *ch = line->first_char; ch; ch = ch->next) {
                const char32_t c = char32_t(ch->c);
                if (c == U' ' || c == U'\t' || c == 0xA0) {
                    flush();
                    continue;
                }
                const fz_rect r = fz_rect_from_quad(ch->quad);
                word.box = word.chars.isEmpty() ? r : fz_union_rect(word.box, r);
                word.chars.append({QString::fromUcs4(&c, 1), r});
            }
            flush();
            if (out.size() > lineStart) {
                out.last().endOfLine = true;
            }
        }
    }
}

fz_stext_page *buildStextPage(fz_context *ctx, fz_page *page)
{
    fz_stext_page *tp = nullptr;
    fz_try(ctx)
    {
        fz_stext_options opts = {};
        tp = fz_new_stext_page_from_page(ctx, page, &opts);
    }
    fz_catch(ctx)
    {
        tp = nullptr;
    }
    return tp;
}
} // namespace

Okular::TextPage *MuPdfGenerator::textPage(Okular::TextRequest *request)
{
    const int number = request->page()->number();

    QMutexLocker lock(&m_mutex);
    if (!m_doc || number < 0 || number >= m_pageBounds.size()) {
        return nullptr;
    }

    fz_page *page = nullptr;
    bool ok = true;
    fz_try(m_ctx)
    {
        page = fz_load_page(m_ctx, m_doc, number);
    }
    fz_catch(m_ctx)
    {
        ok = false;
    }
    if (!ok) {
        return nullptr;
    }

    fz_stext_page *st = buildStextPage(m_ctx, page);
    QList<TextWord> words;
    if (st) {
        collectWords(st->first_block, words);
        fz_drop_stext_page(m_ctx, st);
    }
    fz_drop_page(m_ctx, page);

    const fz_rect bounds = m_pageBounds.at(number);
    auto *textPage = new Okular::TextPage;
    for (int i = 0; i < words.size(); ++i) {
        const TextWord &w = words.at(i);
        for (int j = 0; j < w.chars.size(); ++j) {
            const TextChar &c = w.chars.at(j);
            const bool last = (j == w.chars.size() - 1);
            textPage->append(last && w.endOfLine ? c.text + QLatin1Char('\n') : c.text, normalizedRect(c.box, bounds));
        }
        if (!w.endOfLine) {
            fz_rect gap = w.box;
            if (i + 1 < words.size() && words.at(i + 1).box.x0 > w.box.x1) {
                gap.x0 = w.box.x1;
                gap.x1 = words.at(i + 1).box.x0;
            }
            textPage->append(QStringLiteral(" "), normalizedRect(gap, bounds));
        }
    }
    return textPage;
}

// ---------------------------------------------------------------------------
// Document info and outline

static QString metadata(fz_context *ctx, fz_document *doc, const char *key)
{
    char buf[1024];
    int n = -1;
    fz_try(ctx)
    {
        n = fz_lookup_metadata(ctx, doc, key, buf, sizeof buf);
    }
    fz_catch(ctx)
    {
        n = -1;
    }
    if (n <= 0) {
        return QString();
    }
    return QString::fromUtf8(buf);
}

// PDF date strings look like "D:20240131235959+01'00'".
static QString formatPdfDate(const QString &raw)
{
    QString s = raw;
    if (s.startsWith(QLatin1String("D:"))) {
        s.remove(0, 2);
    }
    const QDateTime dt = QDateTime::fromString(s.left(14), QStringLiteral("yyyyMMddHHmmss"));
    if (!dt.isValid()) {
        return raw;
    }
    return QLocale().toString(dt, QLocale::ShortFormat);
}

Okular::DocumentInfo MuPdfGenerator::generateDocumentInfo(const QSet<Okular::DocumentInfo::Key> &keys) const
{
    Okular::DocumentInfo info;
    QMutexLocker lock(&m_mutex);
    if (!m_doc) {
        return info;
    }
    using Key = Okular::DocumentInfo;

    auto setIf = [&](Key::Key key, const char *mupdfKey, bool isDate = false) {
        if (!keys.contains(key)) {
            return;
        }
        const QString v = metadata(m_ctx, m_doc, mupdfKey);
        if (!v.isEmpty()) {
            info.set(key, isDate ? formatPdfDate(v) : v);
        }
    };
    setIf(Key::Title, "info:Title");
    setIf(Key::Subject, "info:Subject");
    setIf(Key::Author, "info:Author");
    setIf(Key::Keywords, "info:Keywords");
    setIf(Key::Creator, "info:Creator");
    setIf(Key::Producer, "info:Producer");
    setIf(Key::CreationDate, "info:CreationDate", true);
    setIf(Key::ModificationDate, "info:ModDate", true);

    if (keys.contains(Key::MimeType)) {
        info.set(Key::MimeType, QStringLiteral("application/pdf"));
    }
    if (keys.contains(Key::Pages)) {
        info.set(Key::Pages, QString::number(m_pageBounds.size()));
    }
    if (keys.contains(Key::CustomKeys)) {
        const QString fmt = metadata(m_ctx, m_doc, "format");
        if (!fmt.isEmpty()) {
            info.set(QStringLiteral("format"), fmt, QStringLiteral("Format"));
        }
        info.set(QStringLiteral("renderer"), QStringLiteral("MuPDF"), QStringLiteral("Renderer"));
    }
    return info;
}

void MuPdfGenerator::fillSynopsis(QDomNode &parent, fz_outline *outline, int depth)
{
    if (depth > 64) { // guard against malicious/cyclic outlines
        return;
    }
    for (fz_outline *o = outline; o; o = o->next) {
        QDomElement item = m_synopsis->createElement(QString::fromUtf8(o->title ? o->title : ""));

        int pageNumber = -1;
        bool hasPos = false;
        float x = 0;
        float y = 0;
        if (o->uri && !fz_is_external_link(m_ctx, o->uri)) {
            if (!resolveLinkDest(m_ctx, m_doc, o->uri, &pageNumber, &hasPos, &x, &y)) {
                pageNumber = -1;
            }
        }
        if (pageNumber >= 0 && pageNumber < m_pageBounds.size()) {
            item.setAttribute(QStringLiteral("Viewport"), viewportFor(pageNumber, hasPos, x, y, m_pageBounds.at(pageNumber)).toString());
        } else if (o->uri && fz_is_external_link(m_ctx, o->uri)) {
            item.setAttribute(QStringLiteral("URL"), QString::fromUtf8(o->uri));
        }
        item.setAttribute(QStringLiteral("Open"), o->is_open ? QStringLiteral("true") : QStringLiteral("false"));

        parent.appendChild(item);
        if (o->down) {
            fillSynopsis(item, o->down, depth + 1);
        }
    }
}

const Okular::DocumentSynopsis *MuPdfGenerator::generateDocumentSynopsis()
{
    QMutexLocker lock(&m_mutex);
    if (!m_doc) {
        return nullptr;
    }
    if (m_synopsis) {
        return m_synopsis;
    }

    fz_outline *outline = nullptr;
    fz_try(m_ctx)
    {
        outline = fz_load_outline(m_ctx, m_doc);
    }
    fz_catch(m_ctx)
    {
        outline = nullptr;
    }
    if (!outline) {
        return nullptr;
    }

    m_synopsis = new Okular::DocumentSynopsis;
    QDomNode root = *m_synopsis;
    fillSynopsis(root, outline, 0);
    fz_drop_outline(m_ctx, outline);
    return m_synopsis;
}

#include "generator_mupdf.moc"
