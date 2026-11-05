# TextScope agent instructions

Read [README.md](README.md#build) and the
[developer guide](docs/DEVELOPER_GUIDE.md) before configuring, building or testing.
Follow [Development (Debug)](docs/DEVELOPER_GUIDE.md#development-debug) for normal development:
use the `build` directory shared with CMake Tools and enable the example plug-in.
Omit explicit parallel-job arguments from Ninja builds; let Ninja choose its
default concurrency.
Use the separate Release build only for [packaging](docs/RELEASING.md) or
representative [performance benchmarks](docs/DEVELOPER_GUIDE.md#performance-measurements).

Document behavior in the [user guide](docs/USER_GUIDE.md), architecture/build/tests
in the developer guide, distribution workflows in the release guide, and future
work or unverified gates in the [roadmap](docs/ROADMAP.md). Keep the README concise.
