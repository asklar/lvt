using System.Text.Json;

namespace LvtViewer.Services;

/// <summary>Shared JSON options for parsing lvt's MCP payloads.</summary>
public static class JsonDefaults
{
    public static readonly JsonSerializerOptions Options = new()
    {
        PropertyNameCaseInsensitive = true,
    };
}
