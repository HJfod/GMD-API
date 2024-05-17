#include "Shared.hpp"

static void removeNullbytesFromString(std::string& str) {
    for (auto& c : str) {
        if (!c) c = ' ';
    }
}

auto ::handlePlistDataForParsing(std::string& value) -> bool {
    removeNullbytesFromString(value);

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
    return isOldFile;
}
