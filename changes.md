- going to target 64 bit

- going to install openal soft

- installed vcpkg.
- Set the VCPKG_ROOT env var
- set up CmakePresets.json with 
```
{
  "version": 3,
  "configurePresets": [
    {
      "name": "default",
      "cacheVariables": {
        "CMAKE_TOOLCHAIN_FILE": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
      }
    }
  ]
}
```

Installed SDL and OpenAL.  