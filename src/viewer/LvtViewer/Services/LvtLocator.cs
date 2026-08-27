using System;
using System.IO;

namespace LvtViewer.Services;

/// <summary>
/// Locates lvt.exe, the CLI the viewer drives as a subprocess (see README.md
/// for the rationale — the viewer never links lvt_core in-process). Tried in
/// order:
///
///   1. The LVT_EXE environment variable, if set.
///   2. Next to LvtViewer.exe itself (a "copy lvt.exe alongside the viewer"
///      deployment).
///   3. Walking up from the viewer's own build output looking for this
///      repo's CMake build directory ("build\lvt.exe"), which is where a
///      developer's `cmake --build build` puts it. This is what makes
///      `dotnet run` work out of the box in a normal dev checkout without
///      any extra setup.
///   4. %USERPROFILE%\.lvt\lvt.exe (lvt's own plugin/config directory).
///   5. Bare "lvt.exe", so Process.Start falls through to a PATH search.
/// </summary>
public static class LvtLocator
{
    public static string Find()
    {
        var fromEnv = Environment.GetEnvironmentVariable("LVT_EXE");
        if (!string.IsNullOrWhiteSpace(fromEnv) && File.Exists(fromEnv))
            return fromEnv;

        var baseDir = AppContext.BaseDirectory;

        var sibling = Path.Combine(baseDir, "lvt.exe");
        if (File.Exists(sibling))
            return sibling;

        var fromBuildDir = FindInAncestorBuildDirs(baseDir);
        if (fromBuildDir != null)
            return fromBuildDir;

        var userProfile = Environment.GetEnvironmentVariable("USERPROFILE");
        if (!string.IsNullOrWhiteSpace(userProfile))
        {
            var perUser = Path.Combine(userProfile, ".lvt", "lvt.exe");
            if (File.Exists(perUser))
                return perUser;
        }

        // Last resort: let CreateProcess search PATH for a bare filename.
        return "lvt.exe";
    }

    private static string? FindInAncestorBuildDirs(string startDir)
    {
        var dir = new DirectoryInfo(startDir);
        for (var i = 0; i < 10 && dir != null; i++, dir = dir.Parent)
        {
            foreach (var candidate in new[]
                     {
                         Path.Combine(dir.FullName, "build", "lvt.exe"),
                         Path.Combine(dir.FullName, "build", "Release", "lvt.exe"),
                         Path.Combine(dir.FullName, "build", "Debug", "lvt.exe"),
                     })
            {
                if (File.Exists(candidate))
                    return candidate;
            }
        }
        return null;
    }
}
