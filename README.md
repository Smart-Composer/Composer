# Composer

Composer is a Windows-first, fully local digital audio workstation in early development. Its
instrument engine is shared between the application and a standalone VST3 instrument.

The current tree is the native foundation: a reproducible Windows build of the application, the
VST3 instrument and their tests, on C++20, CMake, JUCE and Tracktion Engine.

- [Building and testing on Windows](docs/building.md)
- [Dependencies, pins and licences](docs/dependencies.md)

## Licence

Composer is free software: you can redistribute it and/or modify it under the terms of the
[GNU Affero General Public License](LICENSE) as published by the Free Software Foundation,
either version 3 of the License or (at your option) any later version. It is distributed in the
hope that it will be useful, but without any warranty; without even the implied warranty of
merchantability or fitness for a particular purpose.

Composer is built with JUCE, used under the AGPL-3.0, and Tracktion Engine, used under the
GPL-3.0-or-later, so a distributed build of Composer is covered by the AGPL-3.0. Third-party
components keep their own licences; see [Dependencies, pins and licences](docs/dependencies.md).
