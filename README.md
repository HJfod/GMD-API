# GMD-API

A Geode mod for working with [GMD files](https://fileinfo.com/extension/gmd).

## Installation & Usage

Add `hjfod.gmd-api` into your mod dependencies:

```json
{
    "dependencies": [
        {
            "id": "hjfod.gmd-api",
            "version": ">=v1.0.1",
            "required": true
        }
    ]
}
```

As long as you have at least [Geode CLI v1.4.0](https://github.com/geode-sdk/cli/releases/latest) and [Geode v1.0.0-beta.3](https://github.com/geode-sdk/geode/releases/latest), the dependency will be automatically installed.

Using the dependency works as such:

```cpp
#include <hjfod.gmd-api/include/GMD.hpp>

// exporting a level
gmd::exportLevelAsGmd(level, "path/to/file.gmd");

// importing a level
auto level = gmd::importGmdAsLevel("path/to/file.gmd");
```

Available on both Windows and Mac.
