using System;
using System.Collections;
using System.Collections.Generic;
using System.ComponentModel;
using System.Globalization;
using System.IO.Pipes;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;
using LvtManagedTap;

namespace LvtWpfTap
{
    public static class WpfTreeWalker
    {
        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
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

            public bool TryGet(ulong handle, out DependencyObject value)
            {
                WeakReference reference;
                value = null;
                if (handle > long.MaxValue ||
                    !objects.TryGetValue((long)handle, out reference))
                    return false;
                value = reference.Target as DependencyObject;
                return value != null;
            }

            public void Clear()
            {
                objects.Clear();
            }
        }

        private sealed class WpfPropertyMetadata
        {
            public string DescriptorId;
            public string Name;
            public string DisplayName;
            public string Description;
            public string DeclaringType;
            public string PropertyType;
            public DependencyProperty Property;
            public ManagedScalarDescriptor Scalar;
        }

        private sealed class WpfPropertySchema
        {
            public string SchemaId;
            public List<WpfPropertyMetadata> Properties;
            public Dictionary<string, WpfPropertyMetadata> ByDescriptorId;
        }

        private sealed class WpfPropertyCatalog
        {
            private readonly string connectionId;
            private readonly Dictionary<Type, WpfPropertySchema> schemas =
                new Dictionary<Type, WpfPropertySchema>();
            private int nextSchemaId = 1;
            private int nextDescriptorId = 1;

            public WpfPropertyCatalog(string connectionId)
            {
                this.connectionId = connectionId;
            }

            public WpfPropertySchema GetOrCreate(Type runtimeType)
            {
                WpfPropertySchema existing;
                if (schemas.TryGetValue(runtimeType, out existing))
                    return existing;

                var discovered = new List<WpfPropertyMetadata>();
                var seen = new HashSet<DependencyProperty>();
                foreach (PropertyDescriptor descriptor in TypeDescriptor.GetProperties(runtimeType))
                {
                    DependencyPropertyDescriptor dependencyDescriptor;
                    try
                    {
                        dependencyDescriptor =
                            DependencyPropertyDescriptor.FromProperty(descriptor);
                    }
                    catch
                    {
                        continue;
                    }
                    if (dependencyDescriptor == null ||
                        dependencyDescriptor.DependencyProperty == null)
                        continue;

                    DependencyProperty property =
                        dependencyDescriptor.DependencyProperty;
                    if (property.ReadOnly || descriptor.IsReadOnly || !seen.Add(property))
                        continue;

                    ManagedScalarDescriptor scalar;
                    if (!ManagedProtocol.TryDescribeScalar(
                            property.PropertyType, out scalar))
                        continue;

                    discovered.Add(new WpfPropertyMetadata
                    {
                        Name = property.Name,
                        DisplayName = string.IsNullOrEmpty(descriptor.DisplayName)
                            ? property.Name
                            : descriptor.DisplayName,
                        Description = descriptor.Description ?? "",
                        DeclaringType =
                            property.OwnerType.FullName ?? property.OwnerType.Name,
                        PropertyType =
                            property.PropertyType.FullName ?? property.PropertyType.Name,
                        Property = property,
                        Scalar = scalar,
                    });
                }

                discovered.Sort((left, right) =>
                {
                    int owner = string.Compare(
                        left.DeclaringType, right.DeclaringType,
                        StringComparison.Ordinal);
                    return owner != 0
                        ? owner
                        : string.Compare(left.Name, right.Name, StringComparison.Ordinal);
                });

                string schemaId =
                    connectionId + ":wpf:s" +
                    nextSchemaId++.ToString(CultureInfo.InvariantCulture);
                var byId =
                    new Dictionary<string, WpfPropertyMetadata>(StringComparer.Ordinal);
                foreach (WpfPropertyMetadata property in discovered)
                {
                    property.DescriptorId =
                        connectionId + ":wpf:p" +
                        nextDescriptorId++.ToString(CultureInfo.InvariantCulture);
                    byId[property.DescriptorId] = property;
                }

                var schema = new WpfPropertySchema
                {
                    SchemaId = schemaId,
                    Properties = discovered,
                    ByDescriptorId = byId,
                };
                schemas[runtimeType] = schema;
                return schema;
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

        private sealed class CommandException : Exception
        {
            public CommandException(string message, uint hresult)
                : base(message)
            {
                HResultCode = hresult;
            }

            public uint HResultCode { get; private set; }
        }

        public static int RunServer(IntPtr pipeNamePtr, int pipeNameLength)
        {
            try
            {
                return RunServerImpl(Marshal.PtrToStringUni(pipeNamePtr));
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
                        var catalog = new WpfPropertyCatalog(connectionId);
                        ManagedProtocol.WriteReady(
                            writer, connectionId, AssemblyInstanceId, startCount,
                            "GET_TREE", "GET_PROPERTIES", "SET_PROPERTY",
                            "CLEAR_PROPERTY", "DISCONNECT");

                        for (;;)
                        {
                            string line = reader.ReadLine();
                            if (line == null)
                                break;

                            ManagedRequest request;
                            if (!ManagedProtocol.TryParseRequest(line, out request))
                                continue;
                            if (request.Command == "DISCONNECT")
                            {
                                ManagedProtocol.WriteSuccess(writer, request.Id, "{}");
                                break;
                            }

                            try
                            {
                                string response;
                                switch (request.Command)
                                {
                                    case "GET_TREE":
                                        response = CollectTreeOnDispatcher(registry);
                                        break;
                                    case "GET_PROPERTIES":
                                        response = GetPropertySnapshot(
                                            registry, catalog, request.Arguments);
                                        break;
                                    case "SET_PROPERTY":
                                        response = SetProperty(
                                            registry, catalog, request.Arguments);
                                        break;
                                    case "CLEAR_PROPERTY":
                                        response = ClearProperty(
                                            registry, catalog, request.Arguments);
                                        break;
                                    default:
                                        throw new CommandException(
                                            "Unsupported managed command",
                                            ManagedProtocol.EInvalidArg);
                                }
                                ManagedProtocol.WriteSuccess(writer, request.Id, response);
                            }
                            catch (Exception error)
                            {
                                WriteCommandError(writer, request.Id, error);
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

        private static void WriteCommandError(
            System.IO.StreamWriter writer, string commandId, Exception error)
        {
            error = ManagedProtocol.Unwrap(error);
            var commandError = error as CommandException;
            uint hresult = commandError != null
                ? commandError.HResultCode
                : error is FormatException || error is OverflowException ||
                  error is ArgumentException
                    ? ManagedProtocol.EInvalidArg
                    : error is InvalidOperationException
                        ? ManagedProtocol.InvalidState
                        : ManagedProtocol.EFail;
            ManagedProtocol.WriteError(writer, commandId, error.Message, hresult);
        }

        private static string GetPropertySnapshot(
            ObjectRegistry registry, WpfPropertyCatalog catalog, string arguments)
        {
            ulong handle = ParseHandle(
                ManagedProtocol.SplitArguments(arguments, 1)[0]);
            DependencyObject target = ResolveTarget(registry, handle);
            return InvokeOnDispatcher(() => BuildPropertySnapshot(target, catalog));
        }

        private static string SetProperty(
            ObjectRegistry registry, WpfPropertyCatalog catalog, string arguments)
        {
            string[] parts = ManagedProtocol.SplitArguments(arguments, 3);
            ulong handle = ParseHandle(parts[0]);
            string descriptorId = ManagedProtocol.DecodeHex(parts[1]);
            string value = ManagedProtocol.DecodeHex(parts[2]);
            DependencyObject target = ResolveTarget(registry, handle);
            return InvokeOnDispatcher(() =>
            {
                WpfPropertyMetadata property =
                    ResolveProperty(target, catalog, descriptorId);
                object converted =
                    ManagedProtocol.ConvertScalar(value, property.Scalar);
                target.SetValue(property.Property, converted);
                return BuildMutationResult(
                    target.GetValue(property.Property), false);
            });
        }

        private static string ClearProperty(
            ObjectRegistry registry, WpfPropertyCatalog catalog, string arguments)
        {
            string[] parts = ManagedProtocol.SplitArguments(arguments, 2);
            ulong handle = ParseHandle(parts[0]);
            string descriptorId = ManagedProtocol.DecodeHex(parts[1]);
            DependencyObject target = ResolveTarget(registry, handle);
            return InvokeOnDispatcher(() =>
            {
                WpfPropertyMetadata property =
                    ResolveProperty(target, catalog, descriptorId);
                if (target.ReadLocalValue(property.Property) ==
                    DependencyProperty.UnsetValue)
                {
                    throw new CommandException(
                        "The dependency property has no local value to clear",
                        ManagedProtocol.EInvalidArg);
                }
                target.ClearValue(property.Property);
                return BuildMutationResult(
                    target.GetValue(property.Property), true);
            });
        }

        private static ulong ParseHandle(string text)
        {
            ulong handle;
            if (!ulong.TryParse(
                    text, NumberStyles.None, CultureInfo.InvariantCulture, out handle) ||
                handle == 0)
            {
                throw new CommandException(
                    "The managed element handle is invalid",
                    ManagedProtocol.EInvalidArg);
            }
            return handle;
        }

        private static DependencyObject ResolveTarget(
            ObjectRegistry registry, ulong handle)
        {
            DependencyObject target;
            if (!registry.TryGet(handle, out target))
            {
                throw new CommandException(
                    "The WPF element identity is dead, stale, or unknown",
                    ManagedProtocol.NotFound);
            }
            return target;
        }

        private static WpfPropertyMetadata ResolveProperty(
            DependencyObject target, WpfPropertyCatalog catalog,
            string descriptorId)
        {
            WpfPropertySchema schema = catalog.GetOrCreate(target.GetType());
            WpfPropertyMetadata property;
            if (string.IsNullOrEmpty(descriptorId) ||
                !schema.ByDescriptorId.TryGetValue(descriptorId, out property))
            {
                throw new CommandException(
                    "The WPF property descriptor is unknown, stale, or does not apply to this element",
                    ManagedProtocol.EInvalidArg);
            }
            return property;
        }

        private static string BuildPropertySnapshot(
            DependencyObject target, WpfPropertyCatalog catalog)
        {
            WpfPropertySchema schema = catalog.GetOrCreate(target.GetType());
            var builder = new StringBuilder();
            builder.Append("{\"schemaId\":");
            ManagedProtocol.AppendJsonString(builder, schema.SchemaId);
            builder.Append(",\"descriptors\":[");
            for (int index = 0; index < schema.Properties.Count; index++)
            {
                if (index > 0)
                    builder.Append(',');
                AppendDescriptor(builder, schema.Properties[index]);
            }
            builder.Append("],\"values\":[");
            for (int index = 0; index < schema.Properties.Count; index++)
            {
                if (index > 0)
                    builder.Append(',');
                AppendPropertyValue(builder, target, schema.Properties[index]);
            }
            builder.Append("]}");
            return builder.ToString();
        }

        private static void AppendDescriptor(
            StringBuilder builder, WpfPropertyMetadata property)
        {
            builder.Append("{\"descriptorId\":");
            ManagedProtocol.AppendJsonString(builder, property.DescriptorId);
            builder.Append(",\"name\":");
            ManagedProtocol.AppendJsonString(builder, property.Name);
            builder.Append(",\"displayName\":");
            ManagedProtocol.AppendJsonString(builder, property.DisplayName);
            builder.Append(",\"provider\":\"wpf\",\"framework\":\"wpf\",\"declaringType\":");
            ManagedProtocol.AppendJsonString(builder, property.DeclaringType);
            builder.Append(",\"propertyType\":");
            ManagedProtocol.AppendJsonString(builder, property.PropertyType);
            builder.Append(",\"kind\":");
            ManagedProtocol.AppendJsonString(builder, property.Scalar.KindName);
            builder.Append(",\"choices\":[");
            AppendChoices(builder, property.Scalar);
            builder.Append("],\"writable\":true,\"supportsClear\":true,\"description\":");
            ManagedProtocol.AppendJsonString(
                builder,
                string.IsNullOrEmpty(property.Description)
                    ? "WPF dependency property"
                    : property.Description);
            if (property.Scalar.Kind == ManagedScalarKind.Integer)
                builder.Append(",\"step\":1");
            builder.Append('}');
        }

        private static void AppendChoices(
            StringBuilder builder, ManagedScalarDescriptor scalar)
        {
            bool first = true;
            if (scalar.IsNullable)
            {
                AppendChoice(builder, "", "(null)");
                first = false;
            }
            if (scalar.Kind == ManagedScalarKind.Boolean)
            {
                if (!first) builder.Append(',');
                AppendChoice(builder, "false", "False");
                builder.Append(',');
                AppendChoice(builder, "true", "True");
            }
            else if (scalar.Kind == ManagedScalarKind.Enumeration)
            {
                foreach (string name in ManagedProtocol.EnumNames(scalar.ValueType))
                {
                    if (!first) builder.Append(',');
                    first = false;
                    AppendChoice(builder, name, name);
                }
            }
        }

        private static void AppendChoice(
            StringBuilder builder, string value, string label)
        {
            builder.Append("{\"value\":");
            ManagedProtocol.AppendJsonString(builder, value);
            builder.Append(",\"label\":");
            ManagedProtocol.AppendJsonString(builder, label);
            builder.Append('}');
        }

        private static void AppendPropertyValue(
            StringBuilder builder, DependencyObject target,
            WpfPropertyMetadata property)
        {
            builder.Append("{\"descriptorId\":");
            ManagedProtocol.AppendJsonString(builder, property.DescriptorId);
            try
            {
                object value = target.GetValue(property.Property);
                bool local =
                    target.ReadLocalValue(property.Property) !=
                    DependencyProperty.UnsetValue;
                builder.Append(",\"value\":");
                ManagedProtocol.AppendJsonString(
                    builder, ManagedProtocol.FormatScalar(value));
                builder.Append(",\"runtimeType\":");
                ManagedProtocol.AppendJsonString(
                    builder, ManagedProtocol.RuntimeTypeName(value));
                builder.Append(",\"canClear\":")
                    .Append(local ? "true" : "false");
                builder.Append(",\"overridden\":")
                    .Append(local ? "true" : "false");
                builder.Append(",\"source\":");
                ManagedProtocol.AppendJsonString(
                    builder, WpfValueSource(target, property.Property, local));
                builder.Append(",\"unavailableReason\":\"\",\"readOnlyReason\":\"\"");
            }
            catch (Exception error)
            {
                error = ManagedProtocol.Unwrap(error);
                builder.Append(",\"value\":\"\",\"runtimeType\":\"\",\"canClear\":false,")
                    .Append("\"overridden\":false,\"source\":\"\",\"unavailableReason\":");
                ManagedProtocol.AppendJsonString(builder, error.Message);
                builder.Append(",\"readOnlyReason\":\"\"");
            }
            builder.Append('}');
        }

        private static string WpfValueSource(
            DependencyObject target, DependencyProperty property, bool local)
        {
            if (local)
                return "Local";
            try
            {
                ValueSource source =
                    DependencyPropertyHelper.GetValueSource(target, property);
                var builder = new StringBuilder(source.BaseValueSource.ToString());
                if (source.IsExpression) builder.Append(" expression");
                if (source.IsAnimated) builder.Append(" animated");
                if (source.IsCoerced) builder.Append(" coerced");
                return builder.ToString();
            }
            catch
            {
                return "Effective";
            }
        }

        private static string BuildMutationResult(object value, bool cleared)
        {
            var builder = new StringBuilder();
            builder.Append("{\"value\":");
            ManagedProtocol.AppendJsonString(
                builder, ManagedProtocol.FormatScalar(value));
            builder.Append(",\"runtimeType\":");
            ManagedProtocol.AppendJsonString(
                builder, ManagedProtocol.RuntimeTypeName(value));
            if (cleared)
                builder.Append(",\"cleared\":true");
            builder.Append('}');
            return builder.ToString();
        }

        private static T InvokeOnDispatcher<T>(Func<T> action)
        {
            Application application = Application.Current;
            if (application == null || application.Dispatcher == null)
            {
                throw new CommandException(
                    "WPF Application dispatcher is unavailable",
                    ManagedProtocol.InvalidState);
            }
            Dispatcher dispatcher = application.Dispatcher;
            if (dispatcher.HasShutdownStarted || dispatcher.HasShutdownFinished)
            {
                throw new CommandException(
                    "WPF Application dispatcher is shutting down",
                    ManagedProtocol.InvalidState);
            }

            DispatcherOperation<T> operation =
                dispatcher.InvokeAsync(action, DispatcherPriority.Send);
            if (!operation.Task.Wait(UiTimeoutMilliseconds))
            {
                operation.Abort();
                throw new CommandException(
                    "WPF UI thread did not complete the managed operation",
                    ManagedProtocol.InvalidState);
            }
            return operation.Task.GetAwaiter().GetResult();
        }

        private static string CollectTreeOnDispatcher(ObjectRegistry registry)
        {
            Dictionary<long, WeakReference> snapshot = registry.CreateSnapshot();
            string tree = InvokeOnDispatcher(
                () => WalkAllWindows(registry, snapshot));
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
            builder.Append("{\"managedHandle\":").Append(managedHandle);

            string typeName = element.GetType().FullName ?? element.GetType().Name;
            builder.Append(",\"type\":");
            ManagedProtocol.AppendJsonString(builder, typeName);

            if (element is Window)
            {
                HwndSource source =
                    PresentationSource.FromVisual((Window)element) as HwndSource;
                if (source != null && source.Handle != IntPtr.Zero)
                {
                    builder.Append(",\"hwnd\":");
                    ManagedProtocol.AppendJsonString(
                        builder, source.Handle.ToInt64().ToString("X"));
                }
            }

            FrameworkElement frameworkElement = element as FrameworkElement;
            if (frameworkElement != null)
            {
                if (!string.IsNullOrEmpty(frameworkElement.Name))
                {
                    builder.Append(",\"name\":");
                    ManagedProtocol.AppendJsonString(builder, frameworkElement.Name);
                }

                double width = frameworkElement.ActualWidth;
                double height = frameworkElement.ActualHeight;
                if (width > 0 && height > 0)
                {
                    builder.AppendFormat(
                        CultureInfo.InvariantCulture,
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
                                CultureInfo.InvariantCulture,
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
                {
                    builder.Append(",\"text\":");
                    ManagedProtocol.AppendJsonString(builder, text ?? "");
                }

                if (frameworkElement.Visibility != Visibility.Visible)
                {
                    builder.Append(",\"visible\":false,\"wpf.visibility\":");
                    ManagedProtocol.AppendJsonString(
                        builder, frameworkElement.Visibility.ToString());
                }
                if (!frameworkElement.IsEnabled)
                    builder.Append(",\"enabled\":false");
            }
            else
            {
                FrameworkContentElement contentElement =
                    element as FrameworkContentElement;
                if (contentElement != null && !string.IsNullOrEmpty(contentElement.Name))
                {
                    builder.Append(",\"name\":");
                    ManagedProtocol.AppendJsonString(builder, contentElement.Name);
                }
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
                    if (child != null &&
                        !children.Any(existing => ReferenceEquals(existing, child)))
                        children.Add(child);
                }
            }
            catch
            {
            }
            return children;
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

                foreach (string propertyName in new[] { "Content", "Header" })
                {
                    var property = element.GetType().GetProperty(propertyName);
                    string value = property == null
                        ? null
                        : property.GetValue(element) as string;
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
    }
}
