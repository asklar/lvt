using LvtViewer.Models;
using LvtViewer.Services;

static TreeChangeEventDto Added(string key, string path, string type) => new()
{
    Event = "added",
    Key = key,
    Path = path,
    Element = new ElementDto
    {
        Key = key,
        Type = type,
        Framework = "test",
    },
};

static TreeChangeEventDto Removed(string key, string path) => new()
{
    Event = "removed",
    Key = key,
    Path = path,
};

static void Require(bool condition, string message)
{
    if (!condition)
        throw new InvalidOperationException(message);
}

var tree = new LiveTree();
tree.Apply(Added("root", "0", "Root"));
tree.Apply(Added("old-parent", "0.0", "OldPanel"));
tree.Apply(Added("child", "0.0.0", "Button"));

var root = tree.Roots.Single();
var oldParent = root.Children.Single();
var child = oldParent.Children.Single();

// Native ordering for a same-path parent replacement is: add the replacement
// parent, relocate the provider-stable child, then remove the obsolete parent.
tree.Apply(Added("new-parent", "0.0", "NewPanel"));
var newParent = root.Children.Single(node => node.Key == "new-parent");
tree.Apply(new TreeChangeEventDto
{
    Event = "changed",
    Key = "child",
    Path = "0.0.0",
    Fields = new Dictionary<string, FieldChangeDto>
    {
        ["parentKey"] = new()
        {
            Old = "old-parent",
            New = "new-parent",
        },
    },
});

Require(oldParent.Children.Count == 0,
        "the relocated child remained attached to the obsolete parent");
Require(newParent.Children.Count == 1 &&
        ReferenceEquals(newParent.Children[0], child),
        "the relocated child did not attach to the replacement parent");
Require(ReferenceEquals(child.Parent, newParent),
        "the relocated child's Parent reference was not updated");

tree.Apply(Removed("old-parent", "0.0"));
Require(root.Children.Count == 1 &&
        ReferenceEquals(root.Children[0], newParent),
        "removing the obsolete parent disturbed its replacement");

// This final insertion proves removing the obsolete same-path parent did not
// erase the replacement's _byPath entry.
tree.Apply(Added("sibling", "0.0.1", "TextBlock"));
Require(newParent.Children.Count == 2 &&
        newParent.Children.Any(node => node.Key == "sibling"),
        "the replacement parent no longer owned its path after obsolete removal");

Console.WriteLine("Viewer LiveTree same-path relocation regression passed.");
