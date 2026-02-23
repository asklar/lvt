// WpfTreeWalker.cs — Managed assembly injected into WPF target process.
// Walks the WPF visual tree via VisualTreeHelper and serializes to JSON
// over a named pipe, matching the schema used by the XAML TAP DLL.

using System;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Windows;
using System.Windows.Media;

namespace LvtWpfTap
{
    public static class WpfTreeWalker
    {
        // Delegate type for .NET Core hosting interop
        public delegate int CollectTreeDelegate(IntPtr pipeNamePtr, int pipeNameLength);

        // Entry point for .NET Core hosting (load_assembly_and_get_function_pointer).
        // Takes IntPtr + length matching the component entry point convention.
        public static int CollectTree(IntPtr pipeNamePtr, int pipeNameLength)
        {
            try
            {
                string pipeName = System.Runtime.InteropServices.Marshal.PtrToStringUni(pipeNamePtr);
                return CollectTreeImpl(pipeName);
            }
            catch
            {
                return -1;
            }
        }

        // Entry point for .NET Framework hosting (ExecuteInDefaultAppDomain).
        // Takes a string parameter directly.
        public static int CollectTree(string pipeName)
        {
            return CollectTreeImpl(pipeName);
        }

        private static int CollectTreeImpl(string pipeName)
        {
            try
            {
                // Must run on the WPF Dispatcher thread
                var app = Application.Current;
                if (app == null) return 1;

                string json = null;
                app.Dispatcher.Invoke(() =>
                {
                    json = WalkAllWindows();
                });

                if (json == null) return 2;

                // Extract just the pipe name portion for NamedPipeClientStream
                // Input: "\\.\pipe\lvt_XXXX" -> "lvt_XXXX"
                string shortName = pipeName;
                const string pipePrefix = @"\\.\pipe\";
                if (shortName.StartsWith(pipePrefix))
                    shortName = shortName.Substring(pipePrefix.Length);

                using (var client = new NamedPipeClientStream(".", shortName,
                    PipeDirection.Out))
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

        private static string WalkAllWindows()
        {
            var sb = new StringBuilder();
            sb.Append('[');

            var windows = Application.Current.Windows;
            bool first = true;
            for (int i = 0; i < windows.Count; i++)
            {
                var window = windows[i];
                if (!first) sb.Append(',');
                first = false;
                SerializeElement(sb, window);
            }

            sb.Append(']');
            return sb.ToString();
        }

        private static void SerializeElement(StringBuilder sb, DependencyObject element)
        {
            sb.Append('{');

            string typeName = element.GetType().FullName ?? element.GetType().Name;
            sb.Append("\"type\":\"").Append(EscapeJson(typeName)).Append('"');

            // Name (x:Name)
            if (element is FrameworkElement fe)
            {
                if (!string.IsNullOrEmpty(fe.Name))
                    sb.Append(",\"name\":\"").Append(EscapeJson(fe.Name)).Append('"');

                // Bounds
                double w = fe.ActualWidth;
                double h = fe.ActualHeight;
                if (w > 0 && h > 0)
                {
                    sb.AppendFormat(",\"width\":{0:F1},\"height\":{1:F1}", w, h);

                    // Screen position
                    try
                    {
                        var source = PresentationSource.FromVisual(fe);
                        if (source != null)
                        {
                            Point screenPos = fe.PointToScreen(new Point(0, 0));
                            sb.AppendFormat(",\"offsetX\":{0:F1},\"offsetY\":{1:F1}",
                                screenPos.X, screenPos.Y);
                        }
                    }
                    catch { /* PointToScreen can fail for non-rendered elements */ }
                }

                // Text content for common controls
                string text = GetTextContent(fe);
                if (!string.IsNullOrEmpty(text))
                    sb.Append(",\"text\":\"").Append(EscapeJson(text)).Append('"');

                // Visibility/enabled
                if (fe.Visibility != Visibility.Visible)
                    sb.Append(",\"visible\":false");
                if (!fe.IsEnabled)
                    sb.Append(",\"enabled\":false");
            }

            // Children
            int childCount = VisualTreeHelper.GetChildrenCount(element);
            if (childCount > 0)
            {
                sb.Append(",\"children\":[");
                for (int i = 0; i < childCount; i++)
                {
                    if (i > 0) sb.Append(',');
                    SerializeElement(sb, VisualTreeHelper.GetChild(element, i));
                }
                sb.Append(']');
            }

            sb.Append('}');
        }

        private static string GetTextContent(FrameworkElement fe)
        {
            // Use reflection to get common text properties without hard type deps
            try
            {
                var textProp = fe.GetType().GetProperty("Text");
                if (textProp != null)
                {
                    var val = textProp.GetValue(fe) as string;
                    if (!string.IsNullOrEmpty(val))
                        return val.Length > 200 ? val.Substring(0, 200) : val;
                }

                var contentProp = fe.GetType().GetProperty("Content");
                if (contentProp != null)
                {
                    var val = contentProp.GetValue(fe);
                    if (val is string s && !string.IsNullOrEmpty(s))
                        return s.Length > 200 ? s.Substring(0, 200) : s;
                }

                var headerProp = fe.GetType().GetProperty("Header");
                if (headerProp != null)
                {
                    var val = headerProp.GetValue(fe);
                    if (val is string s && !string.IsNullOrEmpty(s))
                        return s.Length > 200 ? s.Substring(0, 200) : s;
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
