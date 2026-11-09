/*
    Okular backend for PDF documents based on MuPDF

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef OKULAR_GENERATOR_MUPDF_H
#define OKULAR_GENERATOR_MUPDF_H

#include <core/document.h>
#include <core/generator.h>

#include <QDomNode>
#include <QImage>
#include <QList>
#include <QMutex>

extern "C" {
#include <mupdf/fitz.h>
}

class MuPdfGenerator : public Okular::Generator
{
    Q_OBJECT
    Q_INTERFACES(Okular::Generator)

public:
    MuPdfGenerator(QObject *parent, const QVariantList &args);
    ~MuPdfGenerator() override;

    Okular::Document::OpenResult loadDocumentWithPassword(const QString &fileName, QList<Okular::Page *> &pagesVector, const QString &password) override;
    Okular::Document::OpenResult loadDocumentFromDataWithPassword(const QByteArray &fileData, QList<Okular::Page *> &pagesVector, const QString &password) override;

    Okular::DocumentInfo generateDocumentInfo(const QSet<Okular::DocumentInfo::Key> &keys) const override;
    const Okular::DocumentSynopsis *generateDocumentSynopsis() override;
    PageSizeMetric pagesSizeMetric() const override
    {
        return Pixels;
    }

protected:
    bool doCloseDocument() override;
    QImage image(Okular::PixmapRequest *request) override;
    Okular::TextPage *textPage(Okular::TextRequest *request) override;

private:
    bool ensureContext();
    void closeInternal();
    Okular::Document::OpenResult finishOpen(QList<Okular::Page *> &pagesVector, const QString &password);
    QList<Okular::ObjectRect *> generateLinks(fz_page *page, int pageNumber);
    void fillSynopsis(QDomNode &parent, fz_outline *outline, int depth);

    // A MuPDF context must never be used by two threads at once. Okular
    // renders on worker threads, so everything touching m_ctx/m_doc is
    // serialised through this mutex.
    mutable QMutex m_mutex;
    fz_context *m_ctx = nullptr;
    fz_document *m_doc = nullptr;
    QList<fz_rect> m_pageBounds;
    std::vector<bool> m_linksGenerated;
    Okular::DocumentSynopsis *m_synopsis = nullptr;
};

#endif
