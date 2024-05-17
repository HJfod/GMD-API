#include "Shared.hpp"
#include <GMD.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/binding/MusicDownloadManager.hpp>
#include <Geode/utils/JsonValidation.hpp>
#include <Geode/cocos/support/base64.h>

using namespace geode::prelude;
using namespace gmd;

struct ImportGmdList::Impl {
    ghc::filesystem::path path;
    GmdListFileType type = DEFAULT_GMD_LIST_TYPE;

    Impl(ghc::filesystem::path const& path) : path(path) {}
};

ImportGmdList::ImportGmdList(ghc::filesystem::path const& path)
  : m_impl(std::make_unique<Impl>(path)) {}

ImportGmdList ImportGmdList::from(ghc::filesystem::path const& path) {
    return ImportGmdList(path);
}
ImportGmdList::~ImportGmdList() {}

ImportGmdList& ImportGmdList::setType(GmdListFileType type) {
    m_impl->type = type;
    return *this;
}

Result<Ref<GJLevelList>> ImportGmdList::intoList() const {
    GEODE_UNWRAP_INTO(auto data, file::readString(m_impl->path)
        .expect("Unable to read {}: {error}", m_impl->path)
    );
    handlePlistDataForParsing(data);

    auto dict = std::make_unique<DS_Dictionary>();
    if (!dict.get()->loadRootSubDictFromString(data)) {
        return Err("Unable to parse list data");
    }
    dict->stepIntoSubDictWithKey("root");

    auto list = GJLevelList::create();
    list->dataLoaded(dict.get());

    list->m_listType = 2;
    list->m_isEditable = true;

    return Ok(list);
}

struct ExportGmdList::Impl {
    GmdListFileType type = DEFAULT_GMD_LIST_TYPE;
    Ref<GJLevelList> list;

    Impl(GJLevelList* list) : list(list) {}
};

ExportGmdList::ExportGmdList(GJLevelList* list)
  : m_impl(std::make_unique<Impl>(list)) {}

ExportGmdList ExportGmdList::from(GJLevelList* list) {
    return ExportGmdList(list);
}
ExportGmdList::~ExportGmdList() {}

ExportGmdList& ExportGmdList::setType(GmdListFileType type) {
    m_impl->type = type;
    return *this;
}

geode::Result<geode::ByteVector> ExportGmdList::intoBytes() const {
    auto dict = std::make_unique<DS_Dictionary>();
    m_impl->list->encodeWithCoder(dict.get());
    auto data = std::string(dict->saveRootSubDictToString());
    return Ok(ByteVector(data.begin(), data.end()));
}
geode::Result<> ExportGmdList::intoFile(ghc::filesystem::path const& path) const {
    GEODE_UNWRAP_INTO(auto data, this->intoBytes());
    GEODE_UNWRAP(file::writeBinary(path, data));
    return Ok();
}

Result<> gmd::exportListAsGmd(GJLevelList* list, ghc::filesystem::path const& to, GmdListFileType type) {
    return ExportGmdList::from(list).setType(type).intoFile(to);
}
Result<Ref<GJLevelList>> gmd::importGmdAsList(ghc::filesystem::path const& from) {
    return ImportGmdList::from(from).setType(DEFAULT_GMD_LIST_TYPE).intoList();
}
