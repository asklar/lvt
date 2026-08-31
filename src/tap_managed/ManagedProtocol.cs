using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
using System.Threading;

namespace LvtManagedTap
{
    internal sealed class ManagedRequest
    {
        public string Id;
        public string Command;
        public string Arguments;
    }

    internal enum ManagedScalarKind
    {
        String,
        Boolean,
        Integer,
        Number,
        Enumeration,
    }

    internal sealed class ManagedScalarDescriptor
    {
        public Type DeclaredType;
        public Type ValueType;
        public bool IsNullable;
        public ManagedScalarKind Kind;

        public string KindName
        {
            get
            {
                switch (Kind)
                {
                    case ManagedScalarKind.Boolean: return "boolean";
                    case ManagedScalarKind.Integer: return "integer";
                    case ManagedScalarKind.Number: return "number";
                    case ManagedScalarKind.Enumeration: return "enum";
                    default: return "string";
                }
            }
        }
    }

    internal sealed class MutationGate
    {
        // 0 = queued, 1 = running, 2 = cancelled before start, 3 = completed
        private int state;

        public bool TryBegin()
        {
            return Interlocked.CompareExchange(ref state, 1, 0) == 0;
        }

        public bool CancelBeforeStart()
        {
            return Interlocked.CompareExchange(ref state, 2, 0) == 0;
        }

        public bool IsCompleted
        {
            get { return Volatile.Read(ref state) == 3; }
        }

        public void Complete()
        {
            Volatile.Write(ref state, 3);
        }
    }

    internal static class ManagedProtocol
    {
        public const uint EFail = 0x80004005;
        public const uint EInvalidArg = 0x80070057;
        public const uint EAccessDenied = 0x80070005;
        public const uint NotFound = 0x80070490;
        public const uint InvalidState = 0x8007139F;
        public const uint EPending = 0x8000000A;
        public const uint TargetOutsideSession = 0x80040201;

        public static bool TryParseRequest(string line, out ManagedRequest request)
        {
            request = null;
            if (line == null)
                return false;
            string[] parts = line.Split(new[] { '\t' }, 4);
            if (parts.Length < 3 || parts[0] != "REQUEST" ||
                string.IsNullOrEmpty(parts[1]) || string.IsNullOrEmpty(parts[2]))
                return false;
            request = new ManagedRequest
            {
                Id = parts[1],
                Command = parts[2],
                Arguments = parts.Length == 4 ? parts[3] : "",
            };
            return true;
        }

        public static void WriteReady(
            StreamWriter writer, string connectionId, string assemblyInstanceId,
            int serverStartCount, params string[] commands)
        {
            var builder = new StringBuilder();
            builder.Append("READY\t{\"protocol\":1,\"connectionId\":");
            AppendJsonString(builder, connectionId);
            builder.Append(",\"assemblyInstanceId\":");
            AppendJsonString(builder, assemblyInstanceId);
            builder.Append(",\"serverStartCount\":").Append(serverStartCount);
            builder.Append(",\"commands\":[");
            for (int index = 0; index < commands.Length; index++)
            {
                if (index > 0)
                    builder.Append(',');
                AppendJsonString(builder, commands[index]);
            }
            builder.Append("]}");
            writer.WriteLine(builder.ToString());
        }

        public static void WriteSuccess(
            StreamWriter writer, string commandId, string payload)
        {
            writer.WriteLine(
                "RESPONSE\t" + commandId + "\tOK\t" +
                (string.IsNullOrEmpty(payload) ? "{}" : payload));
        }

        public static void WriteError(
            StreamWriter writer, string commandId, string message,
            uint hresult = EFail)
        {
            var builder = new StringBuilder();
            builder.Append("{\"message\":");
            AppendJsonString(builder, string.IsNullOrEmpty(message) ? "Managed command failed" : message);
            builder.Append(",\"hresult\":\"0x")
                .Append(hresult.ToString("X8", CultureInfo.InvariantCulture))
                .Append("\"}");
            writer.WriteLine(
                "RESPONSE\t" + commandId + "\tERROR\t" + builder.ToString());
        }

        public static string[] SplitArguments(string arguments, int expectedCount)
        {
            string[] parts = (arguments ?? "").Split(
                new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length != expectedCount)
                throw new FormatException("The managed command arguments are malformed");
            return parts;
        }

        public static string DecodeHex(string encoded)
        {
            if (encoded == "-")
                return "";
            if (encoded == null || encoded.Length % 2 != 0)
                throw new FormatException("A managed command string is not valid hexadecimal");
            var bytes = new byte[encoded.Length / 2];
            for (int index = 0; index < bytes.Length; index++)
            {
                int high = HexDigit(encoded[index * 2]);
                int low = HexDigit(encoded[index * 2 + 1]);
                if (high < 0 || low < 0)
                    throw new FormatException("A managed command string is not valid hexadecimal");
                bytes[index] = (byte)((high << 4) | low);
            }
            return new UTF8Encoding(false, true).GetString(bytes);
        }

        public static void AppendJsonString(StringBuilder builder, string value)
        {
            builder.Append('"').Append(EscapeJson(value)).Append('"');
        }

        public static string EscapeJson(string value)
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
                            builder.AppendFormat(
                                CultureInfo.InvariantCulture, "\\u{0:X4}", (int)character);
                        else
                            builder.Append(character);
                        break;
                }
            }
            return builder.ToString();
        }

        public static Exception Unwrap(Exception error)
        {
            while ((error is TargetInvocationException ||
                    error is System.AggregateException) &&
                   error.InnerException != null)
                error = error.InnerException;
            return error;
        }

        public static bool TryDescribeScalar(
            Type declaredType, out ManagedScalarDescriptor descriptor)
        {
            descriptor = null;
            if (declaredType == null)
                return false;
            Type valueType = Nullable.GetUnderlyingType(declaredType) ?? declaredType;
            ManagedScalarKind kind;
            if (valueType == typeof(string) || valueType == typeof(char))
                kind = ManagedScalarKind.String;
            else if (valueType == typeof(bool))
                kind = ManagedScalarKind.Boolean;
            else if (valueType == typeof(byte) || valueType == typeof(sbyte) ||
                     valueType == typeof(short) || valueType == typeof(ushort) ||
                     valueType == typeof(int) || valueType == typeof(uint) ||
                     valueType == typeof(long) || valueType == typeof(ulong))
                kind = ManagedScalarKind.Integer;
            else if (valueType == typeof(float) || valueType == typeof(double) ||
                     valueType == typeof(decimal))
                kind = ManagedScalarKind.Number;
            else if (valueType.IsEnum)
                kind = ManagedScalarKind.Enumeration;
            else
                return false;

            descriptor = new ManagedScalarDescriptor
            {
                DeclaredType = declaredType,
                ValueType = valueType,
                IsNullable = Nullable.GetUnderlyingType(declaredType) != null,
                Kind = kind,
            };
            return true;
        }

        public static object ConvertScalar(
            string text, ManagedScalarDescriptor descriptor)
        {
            if (descriptor.IsNullable && string.IsNullOrEmpty(text))
                return null;
            Type type = descriptor.ValueType;
            if (type == typeof(string))
                return text;
            if (type == typeof(char))
            {
                if (text == null || text.Length != 1)
                    throw new FormatException("A character property requires exactly one character");
                return text[0];
            }
            if (type == typeof(bool))
            {
                bool value;
                if (!bool.TryParse(text, out value))
                    throw new FormatException("A Boolean property accepts only true or false");
                return value;
            }
            if (type.IsEnum)
            {
                foreach (object value in Enum.GetValues(type))
                {
                    string name = Enum.GetName(type, value);
                    if (string.Equals(name, text, StringComparison.OrdinalIgnoreCase))
                        return value;
                }
                throw new FormatException(
                    "'" + text + "' is not a declared " + type.Name + " value");
            }

            NumberStyles integer = NumberStyles.AllowLeadingSign;
            NumberStyles number = NumberStyles.Float;
            if (type == typeof(byte)) return byte.Parse(text, integer, CultureInfo.InvariantCulture);
            if (type == typeof(sbyte)) return sbyte.Parse(text, integer, CultureInfo.InvariantCulture);
            if (type == typeof(short)) return short.Parse(text, integer, CultureInfo.InvariantCulture);
            if (type == typeof(ushort)) return ushort.Parse(text, integer, CultureInfo.InvariantCulture);
            if (type == typeof(int)) return int.Parse(text, integer, CultureInfo.InvariantCulture);
            if (type == typeof(uint)) return uint.Parse(text, integer, CultureInfo.InvariantCulture);
            if (type == typeof(long)) return long.Parse(text, integer, CultureInfo.InvariantCulture);
            if (type == typeof(ulong)) return ulong.Parse(text, integer, CultureInfo.InvariantCulture);
            if (type == typeof(decimal))
                return decimal.Parse(text, number, CultureInfo.InvariantCulture);
            if (type == typeof(float))
            {
                float value = float.Parse(text, number, CultureInfo.InvariantCulture);
                if (float.IsNaN(value) || float.IsInfinity(value))
                    throw new FormatException("A numeric property requires a finite value");
                return value;
            }
            if (type == typeof(double))
            {
                double value = double.Parse(text, number, CultureInfo.InvariantCulture);
                if (double.IsNaN(value) || double.IsInfinity(value))
                    throw new FormatException("A numeric property requires a finite value");
                return value;
            }
            throw new FormatException("The property type is not an editable scalar");
        }

        public static string FormatScalar(object value)
        {
            if (value == null)
                return "";
            if (value is bool)
                return (bool)value ? "true" : "false";
            if (value.GetType().IsEnum)
                return Enum.GetName(value.GetType(), value) ?? value.ToString();
            var formattable = value as IFormattable;
            return formattable != null
                ? formattable.ToString(null, CultureInfo.InvariantCulture)
                : value.ToString();
        }

        public static string RuntimeTypeName(object value)
        {
            return value == null ? "null" : (value.GetType().FullName ?? value.GetType().Name);
        }

        public static IEnumerable<string> EnumNames(Type enumType)
        {
            foreach (object value in Enum.GetValues(enumType))
            {
                string name = Enum.GetName(enumType, value);
                if (!string.IsNullOrEmpty(name))
                    yield return name;
            }
        }

        private static int HexDigit(char value)
        {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            if (value >= 'A' && value <= 'F') return value - 'A' + 10;
            return -1;
        }
    }
}
