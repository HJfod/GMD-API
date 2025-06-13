#include "Shared.hpp"
#include <GMD.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/binding/MusicDownloadManager.hpp>
#include <Geode/utils/JsonValidation.hpp>
#include <Geode/cocos/support/base64.h>

using namespace geode::prelude;
using namespace gmd;

static std::string extensionWithoutDot(std::filesystem::path const& path) {
    auto ext = path.extension().string();
    if (ext.size()) {
        return ext.substr(1);
    }
    return "";
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
    std::filesystem::path const& path
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

ImportGmdFile ImportGmdFile::from(std::filesystem::path const& path) {
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
            return file::readString(m_path).mapErr([this](std::string err) { return fmt::format("Unable to read {}: {}", m_path, err); });
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
                        .mapErr([](std::string err) { return fmt::format("Unable to read file: {}", err); })
                );

                // read metadata
                GEODE_UNWRAP_INTO(
                    auto jsonData, unzip.extract("level.meta")
                        .mapErr([](std::string err) { return fmt::format("Unable to read metadata: {}", err); })
                );

                GEODE_UNWRAP_INTO(
                    auto json, matjson::parse(std::string(jsonData.begin(), jsonData.end()))
                        .mapErr([](std::string err) { return fmt::format("Unable to parse metadata: {}", err); })
                );

                JsonExpectedValue root(json, "[level.meta]");
                
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
                            .mapErr([](std::string err) { return fmt::format("Unable to read song file: {}", err); })
                    );

                    std::filesystem::path songTargetPath;
                    if (root.has("song-is-custom").get<bool>()) {
                        songTargetPath = std::string(MusicDownloadManager::sharedState()->pathForSong(
                            std::stoi(songFile.substr(0, songFile.find_first_of(".")))
                        ));
                    } else {
                        songTargetPath = "Resources/" + songFile;
                    }

                    // if we're replacing a file, figure out a different name 
                    // for the old one
                    std::filesystem::path oldSongPath = songTargetPath;
                    while (std::filesystem::exists(oldSongPath)) {
                        // @geode-ignore(unknown-resource)
                        oldSongPath.replace_filename(oldSongPath.stem().string() + "_.mp3");
                    }
                    if (std::filesystem::exists(oldSongPath)) {
                        std::filesystem::rename(songTargetPath, oldSongPath);
                    }
                    (void)file::writeBinary(songTargetPath, songData);
                }

                GEODE_UNWRAP_INTO(
                    auto levelData, unzip.extract("level.data")
                        .mapErr([](std::string err) { return fmt::format("Unable to read level data: {}", err); })
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
    GEODE_UNWRAP_INTO(auto value, getLevelData());

    auto isOldFile = handlePlistDataForParsing(value);

    auto dict = std::make_unique<DS_Dictionary>();
    if (!dict.get()->loadRootSubDictFromString(value)) {
        return Err("Unable to parse level data");
    }
    dict->stepIntoSubDictWithKey("root");

    auto level = GJGameLevel::create();
    level->dataLoaded(dict.get());

    level->m_isEditable = true;
    level->m_levelType = GJLevelType::Editor;

#ifdef GEODE_IS_WINDOWS
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
#endif

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
    auto dict = std::make_unique<DS_Dictionary>();
    m_level->encodeWithCoder(dict.get());
    auto data = dict->saveRootSubDictToString();
    return Ok(std::string(data));
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

            auto json = matjson::Value();
            if (m_includeSong) {
                auto path = std::filesystem::path(std::string(m_level->getAudioFileName()));
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

geode::Result<> ExportGmdFile::intoFile(std::filesystem::path const& path) const {
    GEODE_UNWRAP_INTO(auto data, this->intoBytes());
    GEODE_UNWRAP(file::writeBinary(path, data));
    return Ok();
}

geode::Result<> gmd::exportLevelAsGmd(
    GJGameLevel* level,
    std::filesystem::path const& to,
    GmdFileType type
) {
    return ExportGmdFile::from(level).setType(type).intoFile(to);
}

geode::Result<GJGameLevel*> gmd::importGmdAsLevel(std::filesystem::path const& from) {
    return ImportGmdFile::from(from).inferType().intoLevel();
}

GmdFileKind gmd::getGmdFileKind(std::filesystem::path const& path) {
    auto ext = extensionWithoutDot(path);
    if (gmdListTypeFromString(ext.c_str())) {
        return GmdFileKind::List;
    }
    if (gmdTypeFromString(ext.c_str())) {
        return GmdFileKind::Level;
    }
    return GmdFileKind::None;
}
