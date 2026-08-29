using System;
using System.Collections.Generic;
using LvtViewer.Models;

namespace LvtViewer.ViewModels;

public sealed class PropertyDescriptorSchemaCache
{
    public const int MaximumSchemas = 64;

    private sealed class Entry(
        IReadOnlyList<PropertyDescriptorDto> descriptors,
        long lastUsed)
    {
        public IReadOnlyList<PropertyDescriptorDto> Descriptors { get; } = descriptors;
        public long LastUsed { get; set; } = lastUsed;
    }

    private readonly Dictionary<string, Entry> _entries =
        new(StringComparer.Ordinal);
    private long _clock;

    public int Count => _entries.Count;

    public bool TryGet(
        string schemaId,
        out IReadOnlyList<PropertyDescriptorDto> descriptors)
    {
        if (_entries.TryGetValue(schemaId, out var entry))
        {
            entry.LastUsed = ++_clock;
            descriptors = entry.Descriptors;
            return true;
        }
        descriptors = [];
        return false;
    }

    public void Store(
        string schemaId,
        IReadOnlyList<PropertyDescriptorDto> descriptors,
        params string[] protectedSchemaIds)
    {
        _entries[schemaId] = new Entry(descriptors, ++_clock);
        var protectedIds = new HashSet<string>(
            protectedSchemaIds, StringComparer.Ordinal)
        {
            schemaId,
        };
        while (_entries.Count > MaximumSchemas)
        {
            string? oldestId = null;
            long oldestUse = long.MaxValue;
            foreach (var (id, entry) in _entries)
            {
                if (protectedIds.Contains(id) || entry.LastUsed >= oldestUse)
                    continue;
                oldestId = id;
                oldestUse = entry.LastUsed;
            }
            if (oldestId == null)
                break;
            _entries.Remove(oldestId);
        }
    }

    public void Clear()
    {
        _entries.Clear();
        _clock = 0;
    }
}
