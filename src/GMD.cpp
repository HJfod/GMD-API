#include <GMD.hpp>

#include <Geode/utils/file.hpp>
#include <Geode/binding/MusicDownloadManager.hpp>
#include <Geode/utils/JsonValidation.hpp>

#include <Geode/cocos/support/base64.h>

using namespace geode::prelude;
using namespace gmd;

#define TRY_UNWRAP_INTO(into, fmt, ...) \
    try {\
        into = (__VA_ARGS__);\
    } catch(std::exception& e) {\
        return Err(fmt, e.what());\
    }

static std::string extensionWithoutDot(ghc::filesystem::path const& path) {
    auto ext = path.extension().string();
    if (ext.size()) {
        return ext.substr(1);
    }
    return "";
}

static std::string removeNullbytesFromString(std::string const& str) {
    auto ret = str;
    for (auto& c : ret) {
        if (!c) c = ' ';
    }
    return ret;
}

static bool verifySongFileName(std::string const& name) {
    // Make sure that the song name is .mp3 and the name is parseable as a number
    if (name.ends_with(".mp3")) {
        try {
            (void)std::stoi(name.substr(0, name.size() - 4));
            return true;
        }
        catch(...) {}
    }
    return false;
}

ImportGmdFile::ImportGmdFile(
    ghc::filesystem::path const& path
) : m_path(path) {}

bool ImportGmdFile::tryInferType() {
    if (auto ext = gmdTypeFromString(extensionWithoutDot(m_path).c_str())) {
        m_type = ext.value();
        return true;
    }
    return false;
}

ImportGmdFile& ImportGmdFile::inferType() {
    if (auto ext = gmdTypeFromString(extensionWithoutDot(m_path).c_str())) {
        m_type = ext.value();
    } else {
        m_type = DEFAULT_GMD_TYPE;
    }
    return *this;
}

ImportGmdFile ImportGmdFile::from(ghc::filesystem::path const& path) {
    return ImportGmdFile(path);
}

ImportGmdFile& ImportGmdFile::setImportSong(bool song) {
    m_importSong = song;
    return *this;
}

geode::Result<std::string> ImportGmdFile::getLevelData() const {
    if (!m_type) {
        return Err(
            "No file type set; either it couldn't have been inferred from the "
            "file or the developer of the mod forgot to call inferType"
        );
    }
    switch (m_type.value()) {
        case GmdFileType::Gmd: {
            return file::readString(m_path);
        } break;
    
        case GmdFileType::Lvl: {
            GEODE_UNWRAP_INTO(auto data, file::readBinary(m_path));
            unsigned char* unzippedData;
            auto count = ZipUtils::ccInflateMemory(
                data.data(),
                data.size(),
                &unzippedData
            );
            if (!count) {
                return Err("Unable to decompress level data");
            }
            auto str = std::string(
                reinterpret_cast<const char*>(unzippedData),
                count
            );
            free(unzippedData);
            return Ok(str);
        } break;

        case GmdFileType::Gmd2: {
            try {
                GEODE_UNWRAP_INTO(
                    auto unzip, file::Unzip::create(m_path)
                        .expect("Unable to read file: {error}")
                );

                // read metadata
                GEODE_UNWRAP_INTO(
                    auto jsonData, unzip.extract("level.meta")
                        .expect("Unable to read metadata: {error}")
                );

                json::Value json;
                TRY_UNWRAP_INTO(
                    json, "Unable to parse metadata: {}",
                    json::parse(std::string(jsonData.begin(), jsonData.end()))
                );

                JsonChecker checker(json);
                auto root = checker.root("[level.meta]").obj();
                
                // unzip song
                std::string songFile;
                root.has("song-file").into(songFile);
                if (m_importSong && songFile.size()) {
                    // make sure the song file name is legit. without this check 
                    // it's possible to do arbitary code execution with gmd2
                    if (!verifySongFileName(songFile)) {
                        return Err("Song file name '{}' is invalid!", songFile);
                    }

                    GEODE_UNWRAP_INTO(
                        auto songData, unzip.extract(songFile)
                            .expect("Unable to read song file: {error}")
                    );

                    ghc::filesystem::path songTargetPath;
                    if (root.has("song-is-custom").get<bool>()) {
                        songTargetPath = std::string(MusicDownloadManager::sharedState()->pathForSong(
                            std::stoi(songFile.substr(0, songFile.find_first_of(".")))
                        ));
                    } else {
                        songTargetPath = "Resources/" + songFile;
                    }

                    // if we're replacing a file, figure out a different name 
                    // for the old one
                    ghc::filesystem::path oldSongPath = songTargetPath;
                    while (ghc::filesystem::exists(oldSongPath)) {
                        oldSongPath.replace_filename(oldSongPath.stem().string() + "_.mp3");
                    }
                    if (ghc::filesystem::exists(oldSongPath)) {
                        ghc::filesystem::rename(songTargetPath, oldSongPath);
                    }
                    (void)file::writeBinary(songTargetPath, songData);
                }

                GEODE_UNWRAP_INTO(
                    auto levelData, unzip.extract("level.data")
                        .expect("Unable to read level data: {error}")
                );

                return Ok(std::string(levelData.begin(), levelData.end()));
            } catch(std::exception& e) {
                return Err("Unable to read zip: " + std::string(e.what()));
            }
        } break;

        default: {
            return Err("Unknown file type");
        } break;
    }
}

geode::Result<GJGameLevel*> ImportGmdFile::intoLevel() const {
    auto data = getLevelData();
    if (!data) {
        return Err(data.error());
    }

    std::string value = removeNullbytesFromString(data.value());
    // add gjver if it's missing as otherwise DS_Dictionary fails to load the data
    auto pos = value.substr(0, 100).find("<plist version=\"1.0\">");
    if (pos != std::string::npos) {
        value.replace(pos, 21, "<plist version=\"1.0\" gjver=\"2.0\">");
    }
    bool isOldFile = false;
    if (!value.starts_with("<?xml version")) {
        if (value.substr(0, 100).find("<plist version") == std::string::npos) {
            isOldFile = true;
            value = "<?xml version=\"1.0\"?><plist version=\"1.0\" gjver=\"2.0\"><dict><k>root</k>" +
                value + "</dict></plist>";
        }
        else {
            value = "<?xml version=\"1.0\"?>" + value;
        }
    }

    auto dict = std::make_unique<DS_Dictionary>();
    if (!dict.get()->loadRootSubDictFromString(value)) {
        return Err("Unable to parse level data");
    }
    dict->stepIntoSubDictWithKey("root");

    auto level = GJGameLevel::create();
    level->dataLoaded(dict.get());

    level->m_isEditable = true;
    level->m_levelType = GJLevelType::Editor;

    // old gdshare double base64 encoded the description,
    // so we decode it again
    if (isOldFile && level->m_levelDesc.size()) {
        unsigned char* out = nullptr;
        // we really should add some base64 utils, this is nasty
        auto size = cocos2d::base64Decode(
            reinterpret_cast<unsigned char*>(const_cast<char*>(level->m_levelDesc.c_str())),
            level->m_levelDesc.size(),
            &out
        );
        if (out) {
            auto newDesc = std::string(reinterpret_cast<char*>(out), size);
            free(out);
            level->m_levelDesc = newDesc;
        }
    }

    return Ok(level);
}

ExportGmdFile::ExportGmdFile(GJGameLevel* level) : m_level(level) {}

ExportGmdFile ExportGmdFile::from(GJGameLevel* level) {
    return ExportGmdFile(level);
}

geode::Result<std::string> ExportGmdFile::getLevelData() const {
    if (!m_level) {
        return Err("No level set");
    }
    auto dict = new DS_Dictionary();
    m_level->encodeWithCoder(dict);
    auto data = dict->saveRootSubDictToString();
    delete dict;
    return Ok(data);
}

ExportGmdFile& ExportGmdFile::setIncludeSong(bool song) {
    m_includeSong = song;
    return *this;
}

geode::Result<ByteVector> ExportGmdFile::intoBytes() const {
    if (!m_type) {
        return Err(
            "No file type set; seems like the developer of the mod "
            "forgot to set it"
        );
    }
    switch (m_type.value()) {
        case GmdFileType::Gmd: {
            GEODE_UNWRAP_INTO(auto data, this->getLevelData());
            return Ok(ByteVector(data.begin(), data.end()));
        } break;

        case GmdFileType::Lvl: {
            GEODE_UNWRAP_INTO(auto data, this->getLevelData());
            unsigned char* zippedData = nullptr;
            auto count = ZipUtils::ccDeflateMemory(
                reinterpret_cast<unsigned char*>(data.data()),
                data.size(),
                &zippedData
            );
            if (!count) {
                return Err("Unable to compress level data");
            }
            auto bytes = ByteVector(
                reinterpret_cast<uint8_t*>(zippedData), 
                reinterpret_cast<uint8_t*>(zippedData + count)
            );
            if (zippedData) {
                delete[] zippedData;
            }
            return Ok(bytes);
        } break;

        case GmdFileType::Gmd2: {
            GEODE_UNWRAP_INTO(auto data, this->getLevelData());
            GEODE_UNWRAP_INTO(auto zip, file::Zip::create());

            auto json = json::Value(json::Object());
            if (m_includeSong) {
                auto path = ghc::filesystem::path(
                    std::string(m_level->getAudioFileName())
                );
                json["song-file"] = path.filename().string();
                json["song-is-custom"] = m_level->m_songID;
                GEODE_UNWRAP(zip.addFrom(path));
            }
            GEODE_UNWRAP(zip.add("level.meta", json.dump()));
            GEODE_UNWRAP(zip.add("level.data", data));

            return Ok(zip.getData());
        } break;

        default: {
            return Err("Unknown file type");
        } break;
    }
}

geode::Result<> ExportGmdFile::intoFile(ghc::filesystem::path const& path) const {
    GEODE_UNWRAP_INTO(auto data, this->intoBytes());
    GEODE_UNWRAP(file::writeBinary(path, data));
    return Ok();
}

geode::Result<> gmd::exportLevelAsGmd(
    GJGameLevel* level,
    ghc::filesystem::path const& to,
    GmdFileType type
) {
    return ExportGmdFile::from(level).setType(type).intoFile(to);
}

geode::Result<GJGameLevel*> gmd::importGmdAsLevel(ghc::filesystem::path const& from) {
    return ImportGmdFile::from(from).inferType().intoLevel();
}


