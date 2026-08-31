using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Globalization;
using System.IO.Pipes;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using LvtManagedTap;

namespace LvtWinFormsTap
{
    public static class WinFormsTreeWalker
    {
        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        public delegate int RunServerDelegate(IntPtr pipeNamePtr, int pipeNameLength);

        private const int UiTimeoutMilliseconds = 10000;
        private static readonly Guid AssemblyInstanceGuid = Guid.NewGuid();
        private static readonly string AssemblyInstanceId =
            AssemblyInstanceGuid.ToString("N");
        private static readonly long AssemblyIdentityPrefix =
            Math.Max(
                1,
                BitConverter.ToUInt32(AssemblyInstanceGuid.ToByteArray(), 0) &
                0x7FFFFFFF);
        private static readonly ConditionalWeakTable<Control, Identity> Identities =
            new ConditionalWeakTable<Control, Identity>();
        private static long nextIdentity;
        private static int serverStartCount;

        private delegate bool EnumWindowsCallback(IntPtr hwnd, IntPtr parameter);

        [DllImport("user32.dll")]
        private static extern bool EnumWindows(
            EnumWindowsCallback callback, IntPtr parameter);

        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(
            IntPtr hwnd, out uint processId);

        private sealed class Identity
        {
            public Identity(long value)
            {
                Value = value;
            }

            public long Value { get; private set; }
        }

        private sealed class ObjectEntry
        {
            public WeakReference Target;
            public WeakReference MarshalControl;
        }

        private sealed class ObjectRegistry
        {
            private Dictionary<long, ObjectEntry> objects =
                new Dictionary<long, ObjectEntry>();

            public Dictionary<long, ObjectEntry> CreateSnapshot()
            {
                return new Dictionary<long, ObjectEntry>();
            }

            public long Track(
                Control value, Control marshalControl,
                Dictionary<long, ObjectEntry> snapshot)
            {
                Identity identity = Identities.GetValue(
                    value, ignored => new Identity(NextIdentity()));
                snapshot[identity.Value] = new ObjectEntry
                {
                    Target = new WeakReference(value),
                    MarshalControl = new WeakReference(marshalControl),
                };
                return identity.Value;
            }

            private static long NextIdentity()
            {
                uint local = unchecked((uint)Interlocked.Increment(ref nextIdentity));
                return (AssemblyIdentityPrefix << 32) | local;
            }

            public void Commit(Dictionary<long, ObjectEntry> snapshot)
            {
                objects = snapshot;
            }

            public bool TryGet(
                ulong handle, out Control target, out Control marshalControl)
            {
                target = null;
                marshalControl = null;
                ObjectEntry entry;
                if (handle > long.MaxValue ||
                    !objects.TryGetValue((long)handle, out entry))
                    return false;
                target = entry.Target.Target as Control;
                marshalControl = entry.MarshalControl.Target as Control;
                return target != null && marshalControl != null;
            }

            public void Clear()
            {
                objects.Clear();
            }
        }

        private sealed class WinFormsPropertyMetadata
        {
            public string DescriptorId;
            public string Name;
            public string DisplayName;
            public string Description;
            public string ComponentType;
            public string PropertyType;
            public string ConverterType;
            public ManagedScalarDescriptor Scalar;
            public bool SupportsReset;

            public string Signature
            {
                get
                {
                    return Name + "|" + ComponentType + "|" + PropertyType + "|" +
                           ConverterType + "|" + (SupportsReset ? "reset" : "no-reset");
                }
            }
        }

        private sealed class WinFormsPropertySchema
        {
            public string SchemaId;
            public List<WinFormsPropertyMetadata> Properties;
            public Dictionary<string, WinFormsPropertyMetadata> ByDescriptorId;
        }

        private sealed class SchemaContext
        {
            public WinFormsPropertySchema Schema;
            public Dictionary<string, PropertyDescriptor> Descriptors;
        }

        private sealed class WinFormsPropertyCatalog
        {
            private readonly string connectionId;
            private readonly Dictionary<string, WinFormsPropertySchema> schemas =
                new Dictionary<string, WinFormsPropertySchema>(StringComparer.Ordinal);
            private int nextSchemaId = 1;
            private int nextDescriptorId = 1;

            public WinFormsPropertyCatalog(string connectionId)
            {
                this.connectionId = connectionId;
            }

            public SchemaContext GetContext(Control target)
            {
                Type runtimeType = target.GetType();
                PropertyDescriptorCollection descriptors =
                    TypeDescriptor.GetProperties(target);
                TypeDescriptionProvider provider = TypeDescriptor.GetProvider(target);
                ICustomTypeDescriptor typeDescriptor =
                    provider.GetTypeDescriptor(runtimeType, target);

                var metadata = new List<WinFormsPropertyMetadata>();
                var byName =
                    new Dictionary<string, PropertyDescriptor>(StringComparer.Ordinal);
                foreach (PropertyDescriptor descriptor in descriptors)
                {
                    WinFormsPropertyMetadata property;
                    if (!TryCreateMetadata(runtimeType, descriptor, out property))
                        continue;
                    metadata.Add(property);
                    byName[property.Name] = descriptor;
                }
                metadata.Sort((left, right) =>
                    string.Compare(left.Name, right.Name, StringComparison.Ordinal));

                var fingerprint = new StringBuilder();
                fingerprint.Append(runtimeType.AssemblyQualifiedName)
                    .Append('|')
                    .Append(provider.GetType().AssemblyQualifiedName)
                    .Append('|')
                    .Append(typeDescriptor == null
                        ? ""
                        : typeDescriptor.GetType().AssemblyQualifiedName);
                foreach (WinFormsPropertyMetadata property in metadata)
                    fingerprint.Append('\n').Append(property.Signature);

                string key = fingerprint.ToString();
                WinFormsPropertySchema schema;
                if (!schemas.TryGetValue(key, out schema))
                {
                    string schemaId =
                        connectionId + ":winforms:s" +
                        nextSchemaId++.ToString(CultureInfo.InvariantCulture);
                    var byId =
                        new Dictionary<string, WinFormsPropertyMetadata>(
                            StringComparer.Ordinal);
                    foreach (WinFormsPropertyMetadata property in metadata)
                    {
                        property.DescriptorId =
                            connectionId + ":winforms:p" +
                            nextDescriptorId++.ToString(CultureInfo.InvariantCulture);
                        byId[property.DescriptorId] = property;
                    }
                    schema = new WinFormsPropertySchema
                    {
                        SchemaId = schemaId,
                        Properties = metadata,
                        ByDescriptorId = byId,
                    };
                    schemas[key] = schema;
                }

                return new SchemaContext
                {
                    Schema = schema,
                    Descriptors = byName,
                };
            }

            private static bool TryCreateMetadata(
                Type runtimeType, PropertyDescriptor descriptor,
                out WinFormsPropertyMetadata property)
            {
                property = null;
                if (descriptor == null || !descriptor.IsBrowsable ||
                    descriptor.IsReadOnly || string.IsNullOrEmpty(descriptor.Name))
                    return false;

                PropertyInfo reflected = runtimeType.GetProperty(
                    descriptor.Name,
                    BindingFlags.Instance | BindingFlags.Public);
                if (reflected != null &&
                    (reflected.GetIndexParameters().Length != 0 ||
                     reflected.GetGetMethod() == null ||
                     reflected.GetSetMethod() == null))
                    return false;

                ManagedScalarDescriptor scalar;
                if (!ManagedProtocol.TryDescribeScalar(
                        descriptor.PropertyType, out scalar) ||
                    !IsAuditedConverter(descriptor, scalar))
                    return false;

                Type componentType = descriptor.ComponentType;
                if (componentType != null &&
                    !componentType.IsAssignableFrom(runtimeType))
                    return false;

                property = new WinFormsPropertyMetadata
                {
                    Name = descriptor.Name,
                    DisplayName = string.IsNullOrEmpty(descriptor.DisplayName)
                        ? descriptor.Name
                        : descriptor.DisplayName,
                    Description = descriptor.Description ?? "",
                    ComponentType = componentType == null
                        ? ""
                        : (componentType.FullName ?? componentType.Name),
                    PropertyType =
                        descriptor.PropertyType.FullName ?? descriptor.PropertyType.Name,
                    ConverterType = descriptor.Converter.GetType().AssemblyQualifiedName,
                    Scalar = scalar,
                    SupportsReset = SupportsReset(runtimeType, descriptor),
                };
                return true;
            }

            private static bool IsAuditedConverter(
                PropertyDescriptor descriptor, ManagedScalarDescriptor scalar)
            {
                Type converterType = descriptor.Converter.GetType();
                if (scalar.IsNullable)
                    return converterType == typeof(NullableConverter);
                Type type = scalar.ValueType;
                if (type == typeof(string)) return converterType == typeof(StringConverter);
                if (type == typeof(bool)) return converterType == typeof(BooleanConverter);
                if (type == typeof(char)) return converterType == typeof(CharConverter);
                if (type.IsEnum) return converterType == typeof(EnumConverter);
                if (type == typeof(byte)) return converterType == typeof(ByteConverter);
                if (type == typeof(sbyte)) return converterType == typeof(SByteConverter);
                if (type == typeof(short)) return converterType == typeof(Int16Converter);
                if (type == typeof(ushort)) return converterType == typeof(UInt16Converter);
                if (type == typeof(int)) return converterType == typeof(Int32Converter);
                if (type == typeof(uint)) return converterType == typeof(UInt32Converter);
                if (type == typeof(long)) return converterType == typeof(Int64Converter);
                if (type == typeof(ulong)) return converterType == typeof(UInt64Converter);
                if (type == typeof(float)) return converterType == typeof(SingleConverter);
                if (type == typeof(double)) return converterType == typeof(DoubleConverter);
                if (type == typeof(decimal)) return converterType == typeof(DecimalConverter);
                return false;
            }

            private static bool SupportsReset(
                Type runtimeType, PropertyDescriptor descriptor)
            {
                if (descriptor.Attributes[typeof(DefaultValueAttribute)] != null)
                    return true;
                MethodInfo resetMethod = runtimeType.GetMethod(
                    "Reset" + descriptor.Name,
                    BindingFlags.Instance | BindingFlags.Public |
                    BindingFlags.NonPublic,
                    null, Type.EmptyTypes, null);
                if (resetMethod != null)
                    return true;
                MethodInfo descriptorReset = descriptor.GetType().GetMethod(
                    "ResetValue", new[] { typeof(object) });
                return descriptorReset != null &&
                       descriptorReset.DeclaringType != typeof(PropertyDescriptor) &&
                       descriptor.GetType().Assembly != typeof(PropertyDescriptor).Assembly;
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

        public static int RunServerCore(IntPtr pipeNamePtr, int pipeNameLength)
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
                        var catalog = new WinFormsPropertyCatalog(connectionId);
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
                                        response = CollectTreeOnUiThread(registry);
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
                    : error is ObjectDisposedException || error is InvalidOperationException
                        ? ManagedProtocol.InvalidState
                        : ManagedProtocol.EFail;
            ManagedProtocol.WriteError(writer, commandId, error.Message, hresult);
        }

        private static string GetPropertySnapshot(
            ObjectRegistry registry, WinFormsPropertyCatalog catalog,
            string arguments)
        {
            string[] parts =
                ManagedProtocol.SplitArguments(arguments, 2);
            ulong handle = ParseHandle(parts[0]);
            ulong expectedRoot = ParseExpectedRoot(parts[1]);
            Control target;
            Control marshalControl;
            ResolveTarget(registry, handle, out target, out marshalControl);
            return InvokeOnControl(
                marshalControl,
                () =>
                {
                    EnsureTargetAvailable(target, marshalControl);
                    ValidateTargetRoot(target, expectedRoot);
                    return BuildPropertySnapshot(target, catalog);
                });
        }

        private static string SetProperty(
            ObjectRegistry registry, WinFormsPropertyCatalog catalog,
            string arguments)
        {
            string[] parts = ManagedProtocol.SplitArguments(arguments, 4);
            ulong handle = ParseHandle(parts[0]);
            ulong expectedRoot = ParseExpectedRoot(parts[1]);
            string descriptorId = ManagedProtocol.DecodeHex(parts[2]);
            string value = ManagedProtocol.DecodeHex(parts[3]);
            Control target;
            Control marshalControl;
            ResolveTarget(registry, handle, out target, out marshalControl);
            return InvokeMutationOnControl(
                marshalControl,
                () =>
                {
                    EnsureTargetAvailable(target, marshalControl);
                    ValidateTargetRoot(target, expectedRoot);
                    SchemaContext context = catalog.GetContext(target);
                    WinFormsPropertyMetadata metadata =
                        ResolveProperty(context, descriptorId);
                    PropertyDescriptor descriptor =
                        ResolveCurrentDescriptor(context, metadata);
                    object converted =
                        ManagedProtocol.ConvertScalar(value, metadata.Scalar);
                    descriptor.SetValue(target, converted);
                    ValidateTargetRoot(target, expectedRoot);
                    return BuildMutationResult(descriptor.GetValue(target), false);
                });
        }

        private static string ClearProperty(
            ObjectRegistry registry, WinFormsPropertyCatalog catalog,
            string arguments)
        {
            string[] parts = ManagedProtocol.SplitArguments(arguments, 3);
            ulong handle = ParseHandle(parts[0]);
            ulong expectedRoot = ParseExpectedRoot(parts[1]);
            string descriptorId = ManagedProtocol.DecodeHex(parts[2]);
            Control target;
            Control marshalControl;
            ResolveTarget(registry, handle, out target, out marshalControl);
            return InvokeMutationOnControl(
                marshalControl,
                () =>
                {
                    EnsureTargetAvailable(target, marshalControl);
                    ValidateTargetRoot(target, expectedRoot);
                    SchemaContext context = catalog.GetContext(target);
                    WinFormsPropertyMetadata metadata =
                        ResolveProperty(context, descriptorId);
                    PropertyDescriptor descriptor =
                        ResolveCurrentDescriptor(context, metadata);
                    if (!metadata.SupportsReset ||
                        !descriptor.CanResetValue(target))
                    {
                        throw new CommandException(
                            "The WinForms property cannot currently be reset",
                            ManagedProtocol.EInvalidArg);
                    }
                    descriptor.ResetValue(target);
                    ValidateTargetRoot(target, expectedRoot);
                    return BuildMutationResult(descriptor.GetValue(target), true);
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

        private static ulong ParseExpectedRoot(string text)
        {
            ulong handle;
            if (!ulong.TryParse(
                    text, NumberStyles.None,
                    CultureInfo.InvariantCulture, out handle))
            {
                throw new CommandException(
                    "The expected WinForms session root is invalid",
                    ManagedProtocol.EInvalidArg);
            }
            return handle;
        }

        private static void ValidateTargetRoot(
            Control target, ulong expectedRoot)
        {
            if (expectedRoot == 0)
                return;
            IntPtr expectedHandle =
                unchecked((IntPtr)(long)expectedRoot);
            Control root = Control.FromHandle(expectedHandle);
            bool allowed =
                root != null &&
                !root.IsDisposed &&
                !root.Disposing &&
                root.IsHandleCreated &&
                root.Handle == expectedHandle &&
                (ReferenceEquals(root, target) ||
                 root.Contains(target));
            if (!allowed)
            {
                throw new CommandException(
                    "The WinForms control no longer belongs to the authorized session window",
                    ManagedProtocol.TargetOutsideSession);
            }
        }

        private static void ResolveTarget(
            ObjectRegistry registry, ulong handle,
            out Control target, out Control marshalControl)
        {
            if (!registry.TryGet(handle, out target, out marshalControl))
            {
                throw new CommandException(
                    "The WinForms control identity is dead, stale, or unknown",
                    ManagedProtocol.NotFound);
            }
        }

        private static void EnsureTargetAvailable(
            Control target, Control marshalControl)
        {
            if (marshalControl.IsDisposed || marshalControl.Disposing ||
                !marshalControl.IsHandleCreated)
            {
                throw new CommandException(
                    "The WinForms owning control is disposed or has no handle",
                    ManagedProtocol.InvalidState);
            }
            if (target.IsDisposed || target.Disposing)
            {
                throw new CommandException(
                    "The WinForms target control is disposed",
                    ManagedProtocol.InvalidState);
            }
        }

        private static WinFormsPropertyMetadata ResolveProperty(
            SchemaContext context, string descriptorId)
        {
            WinFormsPropertyMetadata metadata;
            if (string.IsNullOrEmpty(descriptorId) ||
                !context.Schema.ByDescriptorId.TryGetValue(
                    descriptorId, out metadata))
            {
                throw new CommandException(
                    "The WinForms property descriptor is unknown, stale, or does not apply to this control",
                    ManagedProtocol.EInvalidArg);
            }
            return metadata;
        }

        private static PropertyDescriptor ResolveCurrentDescriptor(
            SchemaContext context, WinFormsPropertyMetadata metadata)
        {
            PropertyDescriptor descriptor;
            if (!context.Descriptors.TryGetValue(metadata.Name, out descriptor))
            {
                throw new CommandException(
                    "The WinForms property descriptor is no longer available",
                    ManagedProtocol.InvalidState);
            }
            return descriptor;
        }

        private static string BuildPropertySnapshot(
            Control target, WinFormsPropertyCatalog catalog)
        {
            SchemaContext context = catalog.GetContext(target);
            var builder = new StringBuilder();
            builder.Append("{\"schemaId\":");
            ManagedProtocol.AppendJsonString(builder, context.Schema.SchemaId);
            builder.Append(",\"descriptors\":[");
            for (int index = 0; index < context.Schema.Properties.Count; index++)
            {
                if (index > 0)
                    builder.Append(',');
                AppendDescriptor(builder, context.Schema.Properties[index]);
            }
            builder.Append("],\"values\":[");
            for (int index = 0; index < context.Schema.Properties.Count; index++)
            {
                if (index > 0)
                    builder.Append(',');
                WinFormsPropertyMetadata metadata =
                    context.Schema.Properties[index];
                PropertyDescriptor descriptor;
                context.Descriptors.TryGetValue(metadata.Name, out descriptor);
                AppendPropertyValue(builder, target, metadata, descriptor);
            }
            builder.Append("]}");
            return builder.ToString();
        }

        private static void AppendDescriptor(
            StringBuilder builder, WinFormsPropertyMetadata property)
        {
            builder.Append("{\"descriptorId\":");
            ManagedProtocol.AppendJsonString(builder, property.DescriptorId);
            builder.Append(",\"name\":");
            ManagedProtocol.AppendJsonString(builder, property.Name);
            builder.Append(",\"displayName\":");
            ManagedProtocol.AppendJsonString(builder, property.DisplayName);
            builder.Append(",\"provider\":\"winforms\",\"framework\":\"winforms\",\"declaringType\":");
            ManagedProtocol.AppendJsonString(builder, property.ComponentType);
            builder.Append(",\"propertyType\":");
            ManagedProtocol.AppendJsonString(builder, property.PropertyType);
            builder.Append(",\"kind\":");
            ManagedProtocol.AppendJsonString(builder, property.Scalar.KindName);
            builder.Append(",\"choices\":[");
            AppendChoices(builder, property.Scalar);
            builder.Append("],\"writable\":true,\"supportsClear\":")
                .Append(property.SupportsReset ? "true" : "false");
            builder.Append(",\"description\":");
            ManagedProtocol.AppendJsonString(
                builder,
                string.IsNullOrEmpty(property.Description)
                    ? "WinForms TypeDescriptor property"
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
            StringBuilder builder, Control target,
            WinFormsPropertyMetadata metadata, PropertyDescriptor descriptor)
        {
            builder.Append("{\"descriptorId\":");
            ManagedProtocol.AppendJsonString(builder, metadata.DescriptorId);
            if (descriptor == null)
            {
                builder.Append(",\"value\":\"\",\"runtimeType\":\"\",\"canClear\":false,")
                    .Append("\"overridden\":false,\"source\":\"\",\"unavailableReason\":")
                    .Append("\"The TypeDescriptor property is no longer available\",")
                    .Append("\"readOnlyReason\":\"\"}");
                return;
            }

            try
            {
                object value = descriptor.GetValue(target);
                bool canReset =
                    metadata.SupportsReset && descriptor.CanResetValue(target);
                builder.Append(",\"value\":");
                ManagedProtocol.AppendJsonString(
                    builder, ManagedProtocol.FormatScalar(value));
                builder.Append(",\"runtimeType\":");
                ManagedProtocol.AppendJsonString(
                    builder, ManagedProtocol.RuntimeTypeName(value));
                builder.Append(",\"canClear\":")
                    .Append(canReset ? "true" : "false");
                builder.Append(",\"overridden\":")
                    .Append(canReset ? "true" : "false");
                builder.Append(",\"source\":");
                ManagedProtocol.AppendJsonString(
                    builder, canReset ? "Modified" : "Default");
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

        private static T InvokeOnControl<T>(
            Control marshalControl, Func<T> action)
        {
            var completion = new TaskCompletionSource<T>();
            try
            {
                marshalControl.BeginInvoke(new MethodInvoker(() =>
                {
                    try
                    {
                        completion.SetResult(action());
                    }
                    catch (Exception error)
                    {
                        completion.SetException(error);
                    }
                }));
            }
            catch (Exception error)
            {
                throw new CommandException(
                    "Could not dispatch to the WinForms UI thread: " +
                    ManagedProtocol.Unwrap(error).Message,
                    ManagedProtocol.InvalidState);
            }

            if (!completion.Task.Wait(UiTimeoutMilliseconds))
            {
                throw new CommandException(
                    "WinForms UI thread did not complete the managed operation",
                    ManagedProtocol.InvalidState);
            }
            return completion.Task.GetAwaiter().GetResult();
        }

        private static T InvokeMutationOnControl<T>(
            Control marshalControl, Func<T> action)
        {
            var completion = new TaskCompletionSource<T>();
            var gate = new MutationGate();
            try
            {
                marshalControl.BeginInvoke(new MethodInvoker(() =>
                {
                    if (!gate.TryBegin())
                    {
                        completion.TrySetCanceled();
                        return;
                    }
                    try
                    {
                        completion.TrySetResult(action());
                    }
                    catch (Exception error)
                    {
                        completion.TrySetException(error);
                    }
                    finally
                    {
                        gate.Complete();
                    }
                }));
            }
            catch (Exception error)
            {
                throw new CommandException(
                    "Could not dispatch to the WinForms UI thread: " +
                    ManagedProtocol.Unwrap(error).Message,
                    ManagedProtocol.InvalidState);
            }

            if (!completion.Task.Wait(UiTimeoutMilliseconds))
            {
                if (gate.CancelBeforeStart())
                {
                    throw new CommandException(
                        "WinForms mutation timed out and was cancelled before execution",
                        ManagedProtocol.InvalidState);
                }
                if (completion.Task.IsCompleted || gate.IsCompleted)
                    return completion.Task.GetAwaiter().GetResult();
                throw new CommandException(
                    "WinForms mutation timed out after execution began; its outcome is indeterminate",
                    ManagedProtocol.EPending);
            }
            return completion.Task.GetAwaiter().GetResult();
        }

        private static string CollectTreeOnUiThread(ObjectRegistry registry)
        {
            List<Control> marshalControls = FindMarshalControls();
            if (marshalControls.Count == 0)
            {
                throw new CommandException(
                    "No WinForms UI control with a created handle is available",
                    ManagedProtocol.InvalidState);
            }

            Dictionary<long, ObjectEntry> snapshot = registry.CreateSnapshot();
            var tree = new StringBuilder();
            tree.Append('[');
            for (int index = 0; index < marshalControls.Count; index++)
            {
                Control marshalControl = marshalControls[index];
                string root = InvokeOnControl(
                    marshalControl,
                    () =>
                    {
                        if (marshalControl.IsDisposed ||
                            !marshalControl.IsHandleCreated)
                        {
                            throw new CommandException(
                                "The WinForms UI control is disposed or has no handle",
                                ManagedProtocol.InvalidState);
                        }
                        var builder = new StringBuilder();
                        SerializeControl(
                            builder, marshalControl, marshalControl,
                            registry, snapshot);
                        return builder.ToString();
                    });
                if (index > 0)
                    tree.Append(',');
                tree.Append(root);
            }
            tree.Append(']');
            registry.Commit(snapshot);
            return tree.ToString();
        }

        private static List<Control> FindMarshalControls()
        {
            var result = new List<Control>();
            uint currentProcessId =
                (uint)System.Diagnostics.Process.GetCurrentProcess().Id;
            EnumWindows((hwnd, parameter) =>
            {
                uint windowProcessId;
                GetWindowThreadProcessId(hwnd, out windowProcessId);
                if (windowProcessId != currentProcessId)
                    return true;

                Control control = Control.FromHandle(hwnd);
                if (control == null)
                    return true;
                if (!result.Any(existing => ReferenceEquals(existing, control)))
                    result.Add(control);
                return true;
            }, IntPtr.Zero);
            return result;
        }

        private static void SerializeControl(
            StringBuilder builder, Control control, Control marshalControl,
            ObjectRegistry registry, Dictionary<long, ObjectEntry> snapshot)
        {
            long managedHandle =
                registry.Track(control, marshalControl, snapshot);
            builder.Append("{\"managedHandle\":").Append(managedHandle);
            if (control.IsHandleCreated)
            {
                builder.Append(",\"hwnd\":");
                ManagedProtocol.AppendJsonString(
                    builder, control.Handle.ToInt64().ToString("X"));
            }
            builder.Append(",\"type\":");
            ManagedProtocol.AppendJsonString(
                builder, control.GetType().FullName ?? control.GetType().Name);

            if (!string.IsNullOrEmpty(control.Name))
            {
                builder.Append(",\"name\":");
                ManagedProtocol.AppendJsonString(builder, control.Name);
            }
            if (!string.IsNullOrEmpty(control.Text))
            {
                builder.Append(",\"text\":");
                ManagedProtocol.AppendJsonString(builder, Trim(control.Text));
            }
            if (!control.Visible)
                builder.Append(",\"visible\":false");
            if (!control.Enabled)
                builder.Append(",\"enabled\":false");
            if (control is TextBoxBase)
                builder.Append(",\"readOnly\":")
                    .Append(((TextBoxBase)control).ReadOnly ? "true" : "false");
            if (control is ButtonBase)
                builder.Append(",\"autoSize\":")
                    .Append(((ButtonBase)control).AutoSize ? "true" : "false");

            if (control.Controls.Count > 0)
            {
                builder.Append(",\"children\":[");
                for (int index = 0; index < control.Controls.Count; index++)
                {
                    if (index > 0)
                        builder.Append(',');
                    SerializeControl(
                        builder, control.Controls[index], marshalControl,
                        registry, snapshot);
                }
                builder.Append(']');
            }
            builder.Append('}');
        }

        private static string Trim(string value)
        {
            return value.Length > 200 ? value.Substring(0, 200) : value;
        }
    }
}
