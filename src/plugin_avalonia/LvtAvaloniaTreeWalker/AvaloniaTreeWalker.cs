// AvaloniaTreeWalker.cs — Managed assembly injected into Avalonia target process.
// Walks the Avalonia visual tree and serializes to JSON over a named pipe,
// matching the schema used by the XAML TAP DLL and WPF tree walker.

using System;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;

namespace LvtAvaloniaTreeWalker
{
    public static class AvaloniaTreeWalker
    {
        // Delegate type for .NET Core hosting interop
        public delegate int CollectTreeDelegate(IntPtr pipeNamePtr, int pipeNameLength);

        // Entry point for .NET Core hosting (load_assembly_and_get_function_pointer).
        public static int CollectTree(IntPtr pipeNamePtr, int pipeNameLength)
        {
            try
            {
                string? pipeName = Marshal.PtrToStringUni(pipeNamePtr);
                if (pipeName == null) return -2;
                return CollectTreeImpl(pipeName);
            }
            catch
            {
                return -1;
            }
        }

        private static int CollectTreeImpl(string pipeName)
        {
            try
            {
                // Find the Avalonia Application instance via reflection to avoid
                // hard version dependency. The assemblies are already loaded in the target.
                var avaloniaAssembly = FindAssembly("Avalonia.Base")
                    ?? FindAssembly("Avalonia");
                if (avaloniaAssembly == null) return 1;

                var controlsAssembly = FindAssembly("Avalonia.Controls");
                if (controlsAssembly == null) return 2;

                var desktopAssembly = FindAssembly("Avalonia.Desktop");

                // Get Application.Current
                var appType = controlsAssembly.GetType("Avalonia.Application");
                if (appType == null) return 3;

                var currentProp = appType.GetProperty("Current",
                    System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
                if (currentProp == null) return 4;

                var app = currentProp.GetValue(null);
                if (app == null) return 5;

                // Dispatch to UI thread
                var threadingAssembly = avaloniaAssembly;
                var dispatcherType = threadingAssembly.GetType("Avalonia.Threading.Dispatcher");
                if (dispatcherType == null) return 6;

                var uiThreadProp = dispatcherType.GetProperty("UIThread",
                    System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
                if (uiThreadProp == null) return 7;

                var dispatcher = uiThreadProp.GetValue(null);
                if (dispatcher == null) return 8;

                string? json = null;
                int walkResult = 0;

                // Use Dispatcher.Invoke(Action)
                var invokeMethod = dispatcherType.GetMethod("Invoke",
                    new Type[] { typeof(Action) });
                if (invokeMethod == null) return 9;

                invokeMethod.Invoke(dispatcher, new object[] {
                    new Action(() =>
                    {
                        try
                        {
                            json = WalkAllWindows(app, controlsAssembly, avaloniaAssembly);
                        }
                        catch (Exception ex)
                        {
                            json = "[{\"type\":\"Error\",\"name\":\"" + EscapeJson(ex.Message) + "\"}]";
                            walkResult = 10;
                        }
                    })
                });

                if (json == null) return walkResult != 0 ? walkResult : 11;

                // Send JSON over named pipe
                string shortName = pipeName;
                const string pipePrefix = @"\\.\pipe\";
                if (shortName.StartsWith(pipePrefix))
                    shortName = shortName.Substring(pipePrefix.Length);

                using (var client = new NamedPipeClientStream(".", shortName, PipeDirection.Out))
                {
                    client.Connect(5000);
                    byte[] data = Encoding.UTF8.GetBytes(json);
                    client.Write(data, 0, data.Length);
                    client.Flush();
                }

                return 0;
            }
            catch
            {
                return -1;
            }
        }

        private static System.Reflection.Assembly? FindAssembly(string name)
        {
            foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
            {
                if (asm.GetName().Name == name)
                    return asm;
            }
            return null;
        }

        private static string WalkAllWindows(object app, System.Reflection.Assembly controlsAssembly,
            System.Reflection.Assembly avaloniaAssembly)
        {
            var sb = new StringBuilder();
            sb.Append('[');

            // Get ApplicationLifetime
            var lifetimeProp = app.GetType().GetProperty("ApplicationLifetime");
            if (lifetimeProp == null) return "[]";

            var lifetime = lifetimeProp.GetValue(app);
            if (lifetime == null) return "[]";

            // IClassicDesktopStyleApplicationLifetime.Windows
            var windowsProp = lifetime.GetType().GetProperty("Windows");
            if (windowsProp == null) return "[]";

            var windows = windowsProp.GetValue(lifetime) as System.Collections.IList;
            if (windows == null) return "[]";

            // Get Visual type and VisualChildren property info
            var visualType = avaloniaAssembly.GetType("Avalonia.Visual");

            bool first = true;
            for (int i = 0; i < windows.Count; i++)
            {
                var window = windows[i];
                if (window == null) continue;

                if (!first) sb.Append(',');
                first = false;
                SerializeElement(sb, window, visualType);
            }

            sb.Append(']');
            return sb.ToString();
        }

        private static void SerializeElement(StringBuilder sb, object element,
            Type? visualType)
        {
            sb.Append('{');

            string typeName = element.GetType().FullName ?? element.GetType().Name;
            sb.Append("\"type\":\"").Append(EscapeJson(typeName)).Append('"');

            // Name (from StyledElement.Name)
            var nameProp = element.GetType().GetProperty("Name");
            if (nameProp != null)
            {
                var nameVal = nameProp.GetValue(element) as string;
                if (!string.IsNullOrEmpty(nameVal))
                    sb.Append(",\"name\":\"").Append(EscapeJson(nameVal)).Append('"');
            }

            // Bounds (Visual.Bounds returns Rect with X, Y, Width, Height)
            var boundsProp = element.GetType().GetProperty("Bounds");
            if (boundsProp != null)
            {
                var bounds = boundsProp.GetValue(element);
                if (bounds != null)
                {
                    var boundsType = bounds.GetType();
                    double w = GetDouble(boundsType, bounds, "Width");
                    double h = GetDouble(boundsType, bounds, "Height");

                    if (w > 0 && h > 0)
                    {
                        sb.AppendFormat(",\"width\":{0:F1},\"height\":{1:F1}", w, h);

                        // Get screen position: try PointToScreen
                        try
                        {
                            var pointToScreenMethod = element.GetType().GetMethod("PointToScreen",
                                new Type[] { typeof(Avalonia.Point) });
                            if (pointToScreenMethod != null)
                            {
                                var screenPt = pointToScreenMethod.Invoke(element,
                                    new object[] { new Avalonia.Point(0, 0) });
                                if (screenPt != null)
                                {
                                    var spt = screenPt.GetType();
                                    double sx = GetDouble(spt, screenPt, "X");
                                    double sy = GetDouble(spt, screenPt, "Y");
                                    sb.AppendFormat(",\"offsetX\":{0:F1},\"offsetY\":{1:F1}", sx, sy);
                                }
                            }
                        }
                        catch { /* PointToScreen may fail for non-rendered elements */ }
                    }
                }
            }

            // Text content
            string? text = GetTextContent(element);
            if (!string.IsNullOrEmpty(text))
                sb.Append(",\"text\":\"").Append(EscapeJson(text)).Append('"');

            // IsVisible
            var isVisibleProp = element.GetType().GetProperty("IsVisible");
            if (isVisibleProp != null)
            {
                var val = isVisibleProp.GetValue(element);
                if (val is bool b && !b)
                    sb.Append(",\"visible\":false");
            }

            // IsEnabled
            var isEnabledProp = element.GetType().GetProperty("IsEnabled");
            if (isEnabledProp != null)
            {
                var val = isEnabledProp.GetValue(element);
                if (val is bool b && !b)
                    sb.Append(",\"enabled\":false");
            }

            // Children: Visual.VisualChildren (protected in Avalonia)
            var childrenProp = element.GetType().GetProperty("VisualChildren",
                System.Reflection.BindingFlags.Instance |
                System.Reflection.BindingFlags.Public |
                System.Reflection.BindingFlags.NonPublic);
            if (childrenProp != null)
            {
                var children = childrenProp.GetValue(element) as System.Collections.IList;
                if (children != null && children.Count > 0)
                {
                    sb.Append(",\"children\":[");
                    for (int i = 0; i < children.Count; i++)
                    {
                        if (i > 0) sb.Append(',');
                        var child = children[i];
                        if (child != null)
                            SerializeElement(sb, child, visualType);
                    }
                    sb.Append(']');
                }
            }

            sb.Append('}');
        }

        private static double GetDouble(Type type, object obj, string propName)
        {
            var prop = type.GetProperty(propName);
            if (prop != null)
            {
                var val = prop.GetValue(obj);
                if (val is double d) return d;
            }
            return 0;
        }

        private static string? GetTextContent(object element)
        {
            try
            {
                var textProp = element.GetType().GetProperty("Text");
                if (textProp != null)
                {
                    var val = textProp.GetValue(element) as string;
                    if (!string.IsNullOrEmpty(val))
                        return val.Length > 200 ? val.Substring(0, 200) : val;
                }

                var contentProp = element.GetType().GetProperty("Content");
                if (contentProp != null)
                {
                    var val = contentProp.GetValue(element);
                    if (val is string s && !string.IsNullOrEmpty(s))
                        return s.Length > 200 ? s.Substring(0, 200) : s;
                }

                var headerProp = element.GetType().GetProperty("Header");
                if (headerProp != null)
                {
                    var val = headerProp.GetValue(element);
                    if (val is string s && !string.IsNullOrEmpty(s))
                        return s.Length > 200 ? s.Substring(0, 200) : s;
                }

                // Avalonia Window.Title
                var titleProp = element.GetType().GetProperty("Title");
                if (titleProp != null)
                {
                    var val = titleProp.GetValue(element) as string;
                    if (!string.IsNullOrEmpty(val))
                        return val.Length > 200 ? val.Substring(0, 200) : val;
                }

                // Avalonia Watermark (TextBox)
                var watermarkProp = element.GetType().GetProperty("Watermark");
                if (watermarkProp != null)
                {
                    var val = watermarkProp.GetValue(element) as string;
                    if (!string.IsNullOrEmpty(val))
                        return val.Length > 200 ? val.Substring(0, 200) : val;
                }
            }
            catch { }

            return null;
        }

        private static string EscapeJson(string s)
        {
            if (s == null) return "";
            var sb = new StringBuilder(s.Length);
            foreach (char c in s)
            {
                switch (c)
                {
                    case '"': sb.Append("\\\""); break;
                    case '\\': sb.Append("\\\\"); break;
                    case '\n': sb.Append("\\n"); break;
                    case '\r': sb.Append("\\r"); break;
                    case '\t': sb.Append("\\t"); break;
                    default:
                        if (c < 0x20)
                            sb.AppendFormat("\\u{0:X4}", (int)c);
                        else
                            sb.Append(c);
                        break;
                }
            }
            return sb.ToString();
        }
    }
}
