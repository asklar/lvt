using System;
using System.Collections;
using System.Collections.Generic;
using System.IO.Pipes;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;

namespace LvtWpfTap
{
    public static class WpfTreeWalker
    {
        public delegate int RunServerDelegate(IntPtr pipeNamePtr, int pipeNameLength);

        private const int UiTimeoutMilliseconds = 10000;
        private static readonly string AssemblyInstanceId = Guid.NewGuid().ToString("N");
        private static readonly ConditionalWeakTable<DependencyObject, Identity> Identities =
            new ConditionalWeakTable<DependencyObject, Identity>();
        private static long nextIdentity;
        private static int serverStartCount;

        private sealed class Identity
        {
            public Identity(long value)
            {
                Value = value;
            }

            public long Value { get; private set; }
        }

        private sealed class ObjectRegistry
        {
            private Dictionary<long, WeakReference> objects =
                new Dictionary<long, WeakReference>();

            public Dictionary<long, WeakReference> CreateSnapshot()
            {
                return new Dictionary<long, WeakReference>();
            }

            public long Track(
                DependencyObject value, Dictionary<long, WeakReference> snapshot)
            {
                Identity identity = Identities.GetValue(
                    value, ignored => new Identity(Interlocked.Increment(ref nextIdentity)));
                snapshot[identity.Value] = new WeakReference(value);
                return identity.Value;
            }

            public void Commit(Dictionary<long, WeakReference> snapshot)
            {
                objects = snapshot;
            }

            public bool TryGet(long handle, out DependencyObject value)
            {
                WeakReference reference;
                value = null;
                if (!objects.TryGetValue(handle, out reference))
                    return false;
                value = reference.Target as DependencyObject;
                return value != null;
            }

            public void Clear()
            {
                objects.Clear();
            }
        }

        private sealed class ReferenceComparer : IEqualityComparer<DependencyObject>
        {
            public static readonly ReferenceComparer Instance = new ReferenceComparer();

            public bool Equals(DependencyObject left, DependencyObject right)
            {
                return ReferenceEquals(left, right);
            }

            public int GetHashCode(DependencyObject value)
            {
                return RuntimeHelpers.GetHashCode(value);
            }
        }

        public static int RunServer(IntPtr pipeNamePtr, int pipeNameLength)
        {
            try
            {
                string pipeName = Marshal.PtrToStringUni(pipeNamePtr);
                return RunServerImpl(pipeName);
            }
            catch
            {
                return -1;
            }
        }

        public static int RunServer(string pipeName)
        {
            return RunServerImpl(pipeName);
        }

        private static int RunServerImpl(string pipeName)
        {
            if (string.IsNullOrEmpty(pipeName))
                return 1;

            string shortName = pipeName;
            const string pipePrefix = @"\\.\pipe\";
            if (shortName.StartsWith(pipePrefix, StringComparison.OrdinalIgnoreCase))
                shortName = shortName.Substring(pipePrefix.Length);

            var registry = new ObjectRegistry();
            try
            {
                using (var client = new NamedPipeClientStream(
                    ".", shortName, PipeDirection.InOut, PipeOptions.None))
                {
                    client.Connect(5000);
                    using (var reader = new System.IO.StreamReader(
                        client, new UTF8Encoding(false), false, 4096, true))
                    using (var writer = new System.IO.StreamWriter(
                        client, new UTF8Encoding(false), 4096, true))
                    {
                        writer.AutoFlush = true;
                        int startCount = Interlocked.Increment(ref serverStartCount);
                        string connectionId = Guid.NewGuid().ToString("N");
                        writer.WriteLine(
                            "READY\t{\"protocol\":1,\"connectionId\":\"" + connectionId +
                            "\",\"assemblyInstanceId\":\"" + AssemblyInstanceId +
                            "\",\"serverStartCount\":" + startCount +
                            ",\"commands\":[\"GET_TREE\",\"DISCONNECT\"]}");

                        for (;;)
                        {
                            string line = reader.ReadLine();
                            if (line == null)
                                break;

                            string[] parts = line.Split(new[] { '\t' }, 4);
                            if (parts.Length < 3 || parts[0] != "REQUEST")
                                continue;

                            string commandId = parts[1];
                            string command = parts[2];
                            if (command == "GET_TREE")
                            {
                                try
                                {
                                    string tree = CollectTreeOnDispatcher(registry);
                                    WriteResponse(writer, commandId, "OK", tree);
                                }
                                catch (Exception error)
                                {
                                    WriteResponse(
                                        writer, commandId, "ERROR",
                                        "{\"message\":\"" + EscapeJson(error.Message) + "\"}");
                                }
                            }
                            else if (command == "DISCONNECT")
                            {
                                WriteResponse(writer, commandId, "OK", "{}");
                                break;
                            }
                            else
                            {
                                WriteResponse(
                                    writer, commandId, "ERROR",
                                    "{\"message\":\"unsupported command\"}");
                            }
                        }
                    }
                }
                return 0;
            }
            catch
            {
                return -1;
            }
            finally
            {
                registry.Clear();
            }
        }

        private static void WriteResponse(
            System.IO.StreamWriter writer, string commandId, string status, string payload)
        {
            writer.WriteLine("RESPONSE\t" + commandId + "\t" + status + "\t" + payload);
        }

        private static string CollectTreeOnDispatcher(ObjectRegistry registry)
        {
            Application application = Application.Current;
            if (application == null || application.Dispatcher == null)
                throw new InvalidOperationException("WPF Application dispatcher is unavailable");
            Dispatcher dispatcher = application.Dispatcher;
            if (dispatcher.HasShutdownStarted || dispatcher.HasShutdownFinished)
                throw new InvalidOperationException("WPF Application dispatcher is shutting down");

            Dictionary<long, WeakReference> snapshot = registry.CreateSnapshot();
            DispatcherOperation<string> operation = dispatcher.InvokeAsync(
                () => WalkAllWindows(registry, snapshot), DispatcherPriority.Send);
            if (!operation.Task.Wait(UiTimeoutMilliseconds))
            {
                operation.Abort();
                throw new TimeoutException("WPF UI thread did not complete the tree walk");
            }
            string tree = operation.Task.GetAwaiter().GetResult();
            registry.Commit(snapshot);
            return tree;
        }

        private static string WalkAllWindows(
            ObjectRegistry registry, Dictionary<long, WeakReference> snapshot)
        {
            var builder = new StringBuilder();
            var visited = new HashSet<DependencyObject>(ReferenceComparer.Instance);
            builder.Append('[');

            bool first = true;
            WindowCollection windows = Application.Current.Windows;
            for (int index = 0; index < windows.Count; index++)
            {
                Window window = windows[index];
                if (window == null || visited.Contains(window))
                    continue;
                if (!first)
                    builder.Append(',');
                first = false;
                SerializeElement(builder, window, registry, snapshot, visited);
            }

            builder.Append(']');
            return builder.ToString();
        }

        private static void SerializeElement(
            StringBuilder builder, DependencyObject element, ObjectRegistry registry,
            Dictionary<long, WeakReference> snapshot,
            HashSet<DependencyObject> visited)
        {
            visited.Add(element);
            long managedHandle = registry.Track(element, snapshot);
            builder.Append('{');
            builder.Append("\"managedHandle\":").Append(managedHandle);

            string typeName = element.GetType().FullName ?? element.GetType().Name;
            builder.Append(",\"type\":\"").Append(EscapeJson(typeName)).Append('"');

            if (element is Window window)
            {
                HwndSource source = PresentationSource.FromVisual(window) as HwndSource;
                if (source != null && source.Handle != IntPtr.Zero)
                    builder.Append(",\"hwnd\":\"").Append(
                        source.Handle.ToInt64().ToString("X")).Append('"');
            }

            FrameworkElement frameworkElement = element as FrameworkElement;
            if (frameworkElement != null)
            {
                if (!string.IsNullOrEmpty(frameworkElement.Name))
                    builder.Append(",\"name\":\"").Append(
                        EscapeJson(frameworkElement.Name)).Append('"');

                double width = frameworkElement.ActualWidth;
                double height = frameworkElement.ActualHeight;
                if (width > 0 && height > 0)
                {
                    builder.AppendFormat(
                        System.Globalization.CultureInfo.InvariantCulture,
                        ",\"width\":{0:F1},\"height\":{1:F1}", width, height);
                    try
                    {
                        PresentationSource source =
                            PresentationSource.FromVisual(frameworkElement);
                        if (source != null && source.CompositionTarget != null)
                        {
                            Point screenPosition =
                                frameworkElement.PointToScreen(new Point(0, 0));
                            screenPosition =
                                source.CompositionTarget.TransformFromDevice.Transform(
                                    screenPosition);
                            builder.AppendFormat(
                                System.Globalization.CultureInfo.InvariantCulture,
                                ",\"offsetX\":{0:F1},\"offsetY\":{1:F1}",
                                screenPosition.X, screenPosition.Y);
                        }
                    }
                    catch
                    {
                    }
                }
                else
                {
                    builder.Append(",\"zeroSize\":true");
                }

                bool hasTextProperty;
                string text = GetTextContent(frameworkElement, out hasTextProperty);
                if (hasTextProperty)
                    builder.Append(",\"text\":\"").Append(EscapeJson(text ?? "")).Append('"');

                if (frameworkElement.Visibility != Visibility.Visible)
                {
                    builder.Append(",\"visible\":false");
                    builder.Append(",\"wpf.visibility\":\"").Append(
                        frameworkElement.Visibility.ToString()).Append('"');
                }
                if (!frameworkElement.IsEnabled)
                    builder.Append(",\"enabled\":false");
            }
            else
            {
                FrameworkContentElement contentElement =
                    element as FrameworkContentElement;
                if (contentElement != null && !string.IsNullOrEmpty(contentElement.Name))
                    builder.Append(",\"name\":\"").Append(
                        EscapeJson(contentElement.Name)).Append('"');
            }

            List<DependencyObject> children = GetChildren(element);
            bool wroteChildren = false;
            foreach (DependencyObject child in children)
            {
                if (child == null || visited.Contains(child))
                    continue;
                if (!wroteChildren)
                {
                    builder.Append(",\"children\":[");
                    wroteChildren = true;
                }
                else
                {
                    builder.Append(',');
                }
                SerializeElement(builder, child, registry, snapshot, visited);
            }
            if (wroteChildren)
                builder.Append(']');
            builder.Append('}');
        }

        private static List<DependencyObject> GetChildren(DependencyObject element)
        {
            var children = new List<DependencyObject>();
            try
            {
                int visualCount = VisualTreeHelper.GetChildrenCount(element);
                for (int index = 0; index < visualCount; index++)
                {
                    DependencyObject child = VisualTreeHelper.GetChild(element, index);
                    if (child != null)
                        children.Add(child);
                }
            }
            catch
            {
            }

            try
            {
                IEnumerable logicalChildren = LogicalTreeHelper.GetChildren(element);
                foreach (object value in logicalChildren)
                {
                    DependencyObject child = value as DependencyObject;
                    if (child != null && !ContainsReference(children, child))
                        children.Add(child);
                }
            }
            catch
            {
            }
            return children;
        }

        private static bool ContainsReference(
            List<DependencyObject> values, DependencyObject candidate)
        {
            foreach (DependencyObject value in values)
            {
                if (ReferenceEquals(value, candidate))
                    return true;
            }
            return false;
        }

        private static string GetTextContent(
            FrameworkElement element, out bool hasTextProperty)
        {
            try
            {
                var textProperty = element.GetType().GetProperty("Text");
                if (textProperty != null && textProperty.PropertyType == typeof(string))
                {
                    string value = textProperty.GetValue(element) as string ?? "";
                    hasTextProperty = true;
                    return value.Length > 200 ? value.Substring(0, 200) : value;
                }

                var contentProperty = element.GetType().GetProperty("Content");
                if (contentProperty != null)
                {
                    string value = contentProperty.GetValue(element) as string;
                    if (value != null)
                    {
                        hasTextProperty = true;
                        return value.Length > 200 ? value.Substring(0, 200) : value;
                    }
                }

                var headerProperty = element.GetType().GetProperty("Header");
                if (headerProperty != null)
                {
                    string value = headerProperty.GetValue(element) as string;
                    if (value != null)
                    {
                        hasTextProperty = true;
                        return value.Length > 200 ? value.Substring(0, 200) : value;
                    }
                }
            }
            catch
            {
            }

            hasTextProperty = false;
            return null;
        }

        private static string EscapeJson(string value)
        {
            if (value == null)
                return "";
            var builder = new StringBuilder(value.Length);
            foreach (char character in value)
            {
                switch (character)
                {
                    case '"': builder.Append("\\\""); break;
                    case '\\': builder.Append("\\\\"); break;
                    case '\n': builder.Append("\\n"); break;
                    case '\r': builder.Append("\\r"); break;
                    case '\t': builder.Append("\\t"); break;
                    default:
                        if (character < 0x20)
                            builder.AppendFormat("\\u{0:X4}", (int)character);
                        else
                            builder.Append(character);
                        break;
                }
            }
            return builder.ToString();
        }
    }
}
