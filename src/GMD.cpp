#include "Shared.hpp"
#include <GMD.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/base64.hpp>
#include <Geode/binding/MusicDownloadManager.hpp>
#include <Geode/utils/JsonValidation.hpp>
#include <asp/fs.hpp>

using namespace geode::prelude;
using namespace gmd;

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
    asp::fs::Path const& path
) : m_path(path) {}

bool ImportGmdFile::tryInferType() {
    auto ext = m_path.extension().value_or("");
    if (auto parsed = gmdTypeFromString(std::string(ext).c_str())) {
        m_type = *parsed;
        return true;
    }
    return false;
}

ImportGmdFile& ImportGmdFile::inferType() {
    auto ext = m_path.extension().value_or("");
    if (auto parsed = gmdTypeFromString(std::string(ext).c_str())) {
        m_type = *parsed;
    } else {
        m_type = DEFAULT_GMD_TYPE;
    }
    return *this;
}

ImportGmdFile ImportGmdFile::from(asp::fs::Path const& path) {
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

                    asp::fs::Path songTargetPath;
                    if (root.has("song-is-custom").get<bool>()) {
                        songTargetPath = std::string(MusicDownloadManager::sharedState()->pathForSong(
                            std::stoi(songFile.substr(0, songFile.find_first_of(".")))
                        ));
                    } else {
                        songTargetPath = "Resources/" + songFile;
                    }

                    // if we're replacing a file, figure out a different name 
                    // for the old one
                    if (songTargetPath.exists()) {
                        std::string ext(songTargetPath.extension().value_or(""));
                        std::string base = songTargetPath.string();
                        base = base.substr(0, base.size() - ext.size() - 1);

                        asp::fs::Path backupPath = songTargetPath;
                        for (int n = 0; backupPath.exists(); ++n) {
                            backupPath = asp::fs::Path(base + "_" + std::to_string(n) + "." + ext);
                        }
                        auto _ = asp::fs::rename(songTargetPath, backupPath);
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
    level->m_stars = 0;
    level->m_demon = 0;
    level->m_dailyID = 0;
    level->m_gauntletLevel = false;
    level->m_gauntletLevel2 = false;

    // old gdshare double base64 encoded the description,
    // so we decode it again
    if (isOldFile && level->m_levelDesc.size()) {
        if(auto res = base64::decodeString(level->m_levelDesc)) {
            level->m_levelDesc = res.unwrap();
        }
    }

    // this is required for supporting pre-1.9 gmds
    if(!level->m_levelString.size()) {
        level->m_levelString = dict.get()->getStringForKey("k4");
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
                &zippedData,
                -1
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
                auto path = asp::fs::Path(std::string(m_level->getAudioFileName()));
                if (auto filename = path.filename()) {
                    json["song-file"] = std::string(filename->view());
                } else {
                    json["song-file"] = "";
                }
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

geode::Result<> ExportGmdFile::intoFile(asp::fs::Path const& path) const {
    GEODE_UNWRAP_INTO(auto data, this->intoBytes());
    GEODE_UNWRAP(file::writeBinary(path, data));
    return Ok();
}

geode::Result<> gmd::exportLevelAsGmd(
    GJGameLevel* level,
    asp::fs::Path const& to,
    GmdFileType type
) {
    return ExportGmdFile::from(level).setType(type).intoFile(to);
}

geode::Result<GJGameLevel*> gmd::importGmdAsLevel(asp::fs::Path const& from) {
    return ImportGmdFile::from(from).inferType().intoLevel();
}

GmdFileKind gmd::getGmdFileKind(asp::fs::Path const& path) {
    std::string ext(path.extension().value_or(""));
    if (gmdListTypeFromString(ext.c_str())) {
        return GmdFileKind::List;
    }
    if (gmdTypeFromString(ext.c_str())) {
        return GmdFileKind::Level;
    }
    return GmdFileKind::None;
}
