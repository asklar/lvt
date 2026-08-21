using System.Text.Json;

namespace LvtViewer.Services;

/// <summary>Shared JSON options for parsing lvt's output (dump/watch).</summary>
public static class JsonDefaults
{
    public static readonly JsonSerializerOptions Options = new()
    {
        PropertyNameCaseInsensitive = true,
    };
}
