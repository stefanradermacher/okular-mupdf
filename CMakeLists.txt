cmake_minimum_required(VERSION 3.20)
project(okular-mupdf-generator VERSION 0.1.0 LANGUAGES CXX)

set(QT_MAJOR_VERSION 6)
find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

# Install plugins where Qt (and therefore Okular) looks for them: <libdir>/qt6/plugins
set(KDE_INSTALL_USE_QT_SYS_PATHS ON CACHE BOOL "Install mkspecs, plugins and imports to the Qt 6 locations")
include(KDEInstallDirs)
include(KDECMakeSettings)
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
include(KDECompilerSettings NO_POLICY_SCOPE)
include(FeatureSummary)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Xml)
find_package(KF6 REQUIRED COMPONENTS CoreAddons I18n)
find_package(Okular6 REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MUPDF REQUIRED IMPORTED_TARGET mupdf)

kcoreaddons_add_plugin(okularGenerator_mupdf
    INSTALL_NAMESPACE "okular_generators"
    SOURCES generator_mupdf.cpp)

target_include_directories(okularGenerator_mupdf PRIVATE ${KDE_INSTALL_FULL_INCLUDEDIR}/okular)
target_link_libraries(okularGenerator_mupdf
    Okular::Core
    KF6::CoreAddons
    KF6::I18n
    Qt6::Xml
    PkgConfig::MUPDF)

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
