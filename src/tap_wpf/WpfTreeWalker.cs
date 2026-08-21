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
                            // PointToScreen returns *device* pixels, while
                            // ActualWidth/Height above are device-independent
                            // units. Reporting one of each leaves the origin
                            // scaled by the DPI factor and the size not, so at
                            // 150% an element 613 units across the window is
                            // reported at 920 — outside the window rect lvt
                            // reads, which puts every annotation in the wrong
                            // place and makes the bounds useless to a caller.
                            //
                            // TransformFromDevice converts back to the same
                            // units as the size, which is also the space lvt
                            // works in: it is DPI-unaware, so the window rect
                            // and the UIA bounds it reads are already scaled.
                            // On a DPI-unaware WPF app the transform is
                            // identity, so this is a no-op there.
                            Point screenPos = fe.PointToScreen(new Point(0, 0));
                            var fromDevice = source.CompositionTarget.TransformFromDevice;
                            screenPos = fromDevice.Transform(screenPos);
                            sb.AppendFormat(",\"offsetX\":{0:F1},\"offsetY\":{1:F1}",
                                screenPos.X, screenPos.Y);
                        }
                    }
                    catch { /* PointToScreen can fail for non-rendered elements */ }
                }
                else
                {
                    // w<=0 or h<=0 used to mean total silence here: no width,
                    // no height, no offset, indistinguishable from an element
                    // that was never laid out at all or whose PresentationSource
                    // lookup threw. zeroSize says explicitly "lvt read
                    // ActualWidth/ActualHeight and they really are zero (or
                    // negative before layout)", which is exactly the case a
                    // user investigating a missing/invisible element needs to
                    // be able to tell apart from a different failure.
                    sb.Append(",\"zeroSize\":true");
                }

                // Text content for common controls. hasTextProperty distinguishes
                // "no Text/Content/Header string property exists on this type"
                // (key omitted) from "the property exists and is an empty
                // string" (key emitted as ""), which GetTextContent's caller
                // must be able to tell apart — collapsing them the way a plain
                // IsNullOrEmpty check does means an empty TextBox is
                // unrecoverably identical to a Border with no text at all.
                string text = GetTextContent(fe, out bool hasTextProperty);
                if (hasTextProperty)
                    sb.Append(",\"text\":\"").Append(EscapeJson(text ?? "")).Append('"');

                // Visibility/enabled.
                //
                // "visible":false / absent is kept exactly as before: lvt's
                // generic click-safety check (see lvt_api.cpp centreOf) reads
                // this boolean across every provider, so its shape cannot
                // change here without touching every other provider too.
                //
                // "wpf.visibility" is new and additive, named like
                // "winforms.visible" is for WinForms: WPF has three
                // visibilities, and collapsing Hidden and Collapsed into the
                // same boolean loses the answer to the single most common
                // WPF layout question there is — Hidden still reserves its
                // layout space and Collapsed does not, so "why is there a gap
                // where nothing is showing" has opposite answers depending on
                // which one this was.
                if (fe.Visibility != Visibility.Visible)
                {
                    sb.Append(",\"visible\":false");
                    sb.Append(",\"wpf.visibility\":\"").Append(fe.Visibility.ToString()).Append('"');
                }
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

        private static string GetTextContent(FrameworkElement fe, out bool hasTextProperty)
        {
            // Use reflection to get common text properties without hard type deps.
            //
            // hasTextProperty distinguishes "this element has no string-valued
            // Text/Content/Header at all" from "it has one and it happens to be
            // empty" - SerializeElement needs that to decide whether to omit the
            // "text" key entirely or emit it as "". Content/Header are declared
            // as `object`, so an empty string there is still a real answer;
            // null or a non-string object is not, and we fall through to the
            // next candidate rather than reporting a false "has no text".
            try
            {
                var textProp = fe.GetType().GetProperty("Text");
                if (textProp != null && textProp.PropertyType == typeof(string))
                {
                    var val = textProp.GetValue(fe) as string ?? "";
                    hasTextProperty = true;
                    return val.Length > 200 ? val.Substring(0, 200) : val;
                }

                var contentProp = fe.GetType().GetProperty("Content");
                if (contentProp != null)
                {
                    var val = contentProp.GetValue(fe);
                    if (val is string s)
                    {
                        hasTextProperty = true;
                        return s.Length > 200 ? s.Substring(0, 200) : s;
                    }
                }

                var headerProp = fe.GetType().GetProperty("Header");
                if (headerProp != null)
                {
                    var val = headerProp.GetValue(fe);
                    if (val is string s)
                    {
                        hasTextProperty = true;
                        return s.Length > 200 ? s.Substring(0, 200) : s;
                    }
                }
            }
            catch { }

            hasTextProperty = false;
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
