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

Using the dependency works as such:

```cpp
#include <hjfod.gmd-api/include/GMD.hpp>

// exporting a level
gmd::exportLevelAsGmd(level, "path/to/file.gmd");

// importing a level
auto level = gmd::importGmdAsLevel("path/to/file.gmd");
```

The dependency is available on all platforms!
