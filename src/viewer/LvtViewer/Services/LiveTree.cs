using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using LvtViewer.Models;
using LvtViewer.ViewModels;

namespace LvtViewer.Services;

/// <summary>
/// Maintains the live element tree from a stream of lvt watch events
/// (see watch_diff.h / watch_diff.cpp). This is the sole data source for the
/// viewer's tree and property panel — see README.md for why watch (rather
/// than polling dump, or MCP) was chosen as the live-update mechanism.
///
/// Design: nodes are indexed by lvt's durable "key" (never changes for a
/// given node) in a flat dictionary. Scalar field changes ("changed" events)
/// are applied directly to the existing ElementNodeViewModel's bound
/// properties, which is what makes the UI refresh live without disturbing
/// TreeView selection/expansion. Structural changes (add/remove, or a
/// "changed" event whose "path" field moved — a reorder/reparent) mark the
/// tree dirty and trigger a lightweight rebuild of the parent/child edges,
/// reusing existing view-model instances by key wherever possible.
///
/// lvt's "path" is a dot-separated, depth-first child-index string
/// (element_key.cpp): the root is "0", its children are "0.0", "0.1", ...
/// Because collect_index() visits a node before its children, the very
/// first burst of "added" events a fresh `lvt watch` process emits
/// (run_watch_loop -> snapshot_added_events) already arrives in
/// parent-before-child order — but later ticks diff by sorting on *key*
/// (diff_trees uses std::map&lt;key,...&gt;), so a brand new subtree's
/// "added" events are not guaranteed to arrive parent-first. The rebuild
/// below tolerates arbitrary arrival order because it only runs once all
/// currently-known nodes are indexed by path.
/// </summary>
public sealed class LiveTree
{
    private readonly Dictionary<string, ElementNodeViewModel> _byKey = new();
    private bool _dirty;

    /// <summary>0 or 1 items: the root element of the watched window.</summary>
    public ObservableCollection<ElementNodeViewModel> Roots { get; } = new();

    public void Reset()
    {
        Logger.Log("tree", $"Reset() — clearing {_byKey.Count} known nodes");
        _byKey.Clear();
        Roots.Clear();
        _dirty = false;
    }

    public void Apply(WatchEventDto evt)
    {
        switch (evt.Event)
        {
            case "added":
                ApplyAdded(evt);
                break;
            case "changed":
                ApplyChanged(evt);
                break;
            case "removed":
                ApplyRemoved(evt);
                break;
        }
        // Deliberately does NOT rebuild here — see Flush(). watch emits one
        // JSON line per *element*, not one per tick: a single tick (let
        // alone the very first burst, which is the whole initial tree) can
        // be hundreds or thousands of lines. Rebuilding after every single
        // one of them was measured live doing 5454 full-hierarchy rebuilds
        // in one ~140s session — the real cause of "the tree refreshes as I
        // navigate": it was not any one rebuild corrupting state, it was
        // WPF re-laying-out the whole TreeView many times a second while
        // the target's own animated/live content kept ticking, which reads
        // exactly like the view resetting even though the underlying model
        // was fine. The caller is expected to call Flush() once after
        // draining all events currently available, not after each one.
    }

    /// <summary>
    /// Rebuilds the hierarchy if anything Applied since the last Flush
    /// actually requires it. Call this once per drained batch of events —
    /// never per individual event — so a burst of N related changes costs
    /// one rebuild instead of N.
    /// </summary>
    public void Flush()
    {
        if (!_dirty)
            return;
        Logger.Log("tree", $"flush — rebuilding hierarchy over {_byKey.Count} known nodes");
        RebuildHierarchy();
        _dirty = false;
    }

    private ElementNodeViewModel GetOrCreate(string key)
    {
        if (!_byKey.TryGetValue(key, out var node))
        {
            node = new ElementNodeViewModel { Key = key };
            _byKey[key] = node;
        }
        return node;
    }

    private void ApplyAdded(WatchEventDto evt)
    {
        if (evt.Element == null)
            return;
        var node = GetOrCreate(evt.Key);
        node.UpdateFrom(evt.Element, evt.Key, evt.Path);
        Logger.Log("tree", $"added key={evt.Key} path={evt.Path} -> dirty");
        _dirty = true;
    }

    private void ApplyChanged(WatchEventDto evt)
    {
        if (!_byKey.TryGetValue(evt.Key, out var node) || evt.Fields == null)
            return;

        foreach (var (fieldName, change) in evt.Fields)
        {
            if (fieldName == "path")
            {
                Logger.Log("tree", $"changed key={evt.Key} path {node.Path} -> {change.New} -> dirty");
                node.Path = change.New;
                _dirty = true;
            }
            else if (fieldName == "bounds")
            {
                node.SetBoundsFromString(change.New);
            }
            else if (fieldName.StartsWith("properties.", StringComparison.Ordinal))
            {
                node.SetProperty(fieldName["properties.".Length..], change.New);
            }
            else
            {
                node.SetScalarField(fieldName, change.New);
            }
        }
    }

    private void ApplyRemoved(WatchEventDto evt)
    {
        if (_byKey.Remove(evt.Key))
        {
            Logger.Log("tree", $"removed key={evt.Key} -> dirty");
            _dirty = true;
        }
    }

    /// <summary>
    /// Recomputes parent/child edges from each node's current Path, reusing
    /// existing ElementNodeViewModel instances so the TreeView keeps
    /// selection and expansion state for nodes that did not move.
    /// </summary>
    private void RebuildHierarchy()
    {
        var pathToKey = new Dictionary<string, string>(_byKey.Count);
        foreach (var (key, node) in _byKey)
            pathToKey[node.Path] = key;

        var childrenOfPath = new Dictionary<string, List<ElementNodeViewModel>>();
        string? rootKey = null;

        foreach (var (key, node) in _byKey)
        {
            if (node.Path == "0")
            {
                rootKey = key;
                continue;
            }

            var lastDot = node.Path.LastIndexOf('.');
            var parentPath = lastDot < 0 ? "" : node.Path[..lastDot];
            if (!childrenOfPath.TryGetValue(parentPath, out var list))
                childrenOfPath[parentPath] = list = new List<ElementNodeViewModel>();
            list.Add(node);
        }

        // Order each parent's children by their own path's last numeric
        // segment, so display order matches the original DFS child order
        // rather than dictionary iteration order.
        foreach (var list in childrenOfPath.Values)
        {
            list.Sort((a, b) =>
            {
                var ai = ChildIndex(a.Path);
                var bi = ChildIndex(b.Path);
                return ai.CompareTo(bi);
            });
        }

        void Apply(ElementNodeViewModel node)
        {
            var desired = childrenOfPath.TryGetValue(node.Path, out var list)
                ? list
                : new List<ElementNodeViewModel>();
            SyncCollection(node.Children, desired);
            foreach (var child in desired)
            {
                child.Parent = node;
                Apply(child);
            }
        }

        if (rootKey != null && _byKey.TryGetValue(rootKey, out var root))
        {
            root.Parent = null;
            root.IsExpanded = true;
            SyncCollection(Roots, new List<ElementNodeViewModel> { root });
            Apply(root);
        }
        else
        {
            Roots.Clear();
        }
    }

    private static int ChildIndex(string path)
    {
        var lastDot = path.LastIndexOf('.');
        var segment = lastDot < 0 ? path : path[(lastDot + 1)..];
        return int.TryParse(segment, out var value) ? value : 0;
    }

    /// <summary>
    /// Updates <paramref name="existing"/> in place to match
    /// <paramref name="desired"/> by object identity, minimizing removes/
    /// inserts/moves so WPF's ItemContainerGenerator can preserve as much
    /// TreeViewItem state (expansion) as possible.
    /// </summary>
    private static void SyncCollection(ObservableCollection<ElementNodeViewModel> existing,
                                        IReadOnlyList<ElementNodeViewModel> desired)
    {
        // Remove items no longer present.
        for (var i = existing.Count - 1; i >= 0; i--)
        {
            if (!desired.Contains(existing[i]))
                existing.RemoveAt(i);
        }

        // Insert/move so the final order and membership matches `desired`.
        for (var i = 0; i < desired.Count; i++)
        {
            var item = desired[i];
            var currentIndex = existing.IndexOf(item);
            if (currentIndex < 0)
            {
                existing.Insert(i, item);
            }
            else if (currentIndex != i)
            {
                existing.Move(currentIndex, i);
            }
        }
    }
}
