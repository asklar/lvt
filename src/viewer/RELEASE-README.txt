lvt Viewer
==========

Requirements
------------

- Windows x64
- .NET 10 Desktop Runtime:
  https://dotnet.microsoft.com/download/dotnet/10.0

Run
---

Extract the whole archive, then run LvtViewer.exe.

Keep the included files together. The viewer launches the matching lvt.exe
beside it, and that executable needs its TAP DLLs, managed tree walkers, and
plugins in the included relative locations.

This viewer archive is version-matched and self-contained with respect to lvt.
You do not need to download the separate lvt command-line archive unless you
also want a smaller CLI-only installation.

The viewer is framework-dependent to keep this download small. If Windows says
that a required .NET runtime is missing, install the .NET 10 Desktop Runtime
from the link above and launch LvtViewer.exe again.
