# Chromium Plugin — Chrome/Edge DOM Inspection

The Chromium plugin lets lvt inspect the DOM tree of web pages in Google Chrome and Microsoft Edge. It works via a browser extension that communicates with lvt through Chrome's [Native Messaging](https://developer.chrome.com/docs/extensions/develop/concepts/native-messaging) protocol.

## How it works

```
lvt.exe → plugin DLL → named pipe → native host → Chrome extension → chrome.debugger (CDP) → DOM
```

1. **lvt** detects Chrome/Edge by checking for `chrome.dll` or `msedge.dll` in the target process
2. The **plugin** connects to a named pipe served by the native messaging host
3. The **native messaging host** relays the request to the browser extension
4. The **plugin** can request the extension's tab list and select a tab by URL/title before the DOM walk
5. The **extension** uses the `chrome.debugger` API (Chrome DevTools Protocol) to walk the DOM tree of the selected tab (or the active tab when no tab selector is provided)
6. The DOM tree is returned as an lvt element tree with bounds, properties, and text content

## Prerequisites

- Google Chrome 110+ or Microsoft Edge 110+
- lvt built with the chromium plugin (included by default)

## Installation

### 1. Register the native messaging host

```powershell
build\plugins\chromium\lvt_chromium_host.exe --register
```

This creates registry entries for both Chrome and Edge and writes a `com.lvt.chromium.json` manifest file.

### 2. Load the browser extension

1. Open `chrome://extensions` (Chrome) or `edge://extensions` (Edge)
2. Enable **Developer mode** (toggle in top-right)
3. Click **Load unpacked**
4. Select the `build/plugins/chromium/extension/` directory

The extension icon should appear in the toolbar. The shipped extension includes `icons/icon16.png`, `icons/icon48.png`, and `icons/icon128.png`, so Chrome/Edge should load it without "Could not load icon" errors. The extension will automatically connect to the native messaging host.

## Usage

```powershell
# Inspect Chrome
lvt --name chrome

# Inspect Edge
lvt --name msedge

# Output as XML
lvt --name chrome --format xml

# Capture screenshot with element annotations
lvt --name chrome --screenshot page.png

# Select a Chromium tab by URL or title substring while targeting the browser
lvt --name chrome --title "github.com/asklar/lvt"

# Prefix with url: or title: to restrict the match; * and ? wildcards are supported
lvt --name msedge --title "title:Dashboard"
```

When `--title` is the only target selector, it still selects a top-level window by
title. When it is combined with `--name`, `--pid`, or `--hwnd` for a Chromium
browser, the Chromium plugin also uses it to select the tab whose URL or title
matches the provided substring/pattern. If no tab matches, lvt prints a clear
`lvt-chromium: No Chromium tab matches '...'` error.

## Manual live E2E check

From the build output directory:

```powershell
build\plugins\chromium\lvt_chromium_host.exe --register
```

Then open Microsoft Edge, go to `edge://extensions`, enable **Developer mode**, choose **Load unpacked**, and select `build\plugins\chromium\extension`. Open a normal web page in the active tab and run:

```powershell
build\lvt.exe --name msedge
```

The output should include Chromium DOM elements from the active tab. Use `--format xml` or `--screenshot page.png` for alternate verification.

## What you get

The DOM tree is mapped to lvt elements:

| DOM concept | lvt element field |
|-------------|-------------------|
| Tag name (`DIV`, `SPAN`) | `type` |
| Tag name (lowercase) | `className` |
| Text content | `text` |
| HTML attributes | `properties` |
| `getBoundingClientRect()` | `bounds` |
| Child elements | `children` |

Framework name is reported as `"chromium (Chrome)"` or `"chromium (Edge)"`.

### Example output (JSON)

```json
{
  "id": "e0",
  "type": "Window",
  "framework": "win32",
  "children": [
    {
      "id": "e1",
      "type": "HTML",
      "framework": "chromium (Chrome)",
      "children": [
        {
          "id": "e2",
          "type": "BODY",
          "framework": "chromium (Chrome)",
          "bounds": { "x": 0, "y": 0, "width": 1920, "height": 3000 },
          "properties": { "class": "main-content" },
          "children": [
            {
              "id": "e3",
              "type": "DIV",
              "properties": { "id": "app", "class": "container" },
              "text": "Hello World"
            }
          ]
        }
      ]
    }
  ]
}
```

### Example output (XML)

```xml
<Window id="e0" framework="win32">
  <HTML id="e1" framework="chromium (Chrome)">
    <BODY id="e2" bounds="0,0,1920,3000" class="main-content">
      <DIV id="e3" html-id="app" class="container" text="Hello World" />
    </BODY>
  </HTML>
</Window>
```

## Architecture

### Browser Extension (Manifest V3)

- **Service worker** (`service-worker.js`): Connects to the native messaging host, dispatches tab-list and DOM requests, uses `chrome.debugger` API for DOM walking
- Works on both Chrome and Edge (same Chromium extension format)
- Uses `chrome.debugger.sendCommand("DOM.getDocument", {depth: -1, pierce: true})` for full DOM including shadow DOM
- Gets element bounding boxes via `DOM.getBoxModel`

### Native Messaging Host (`lvt_chromium_host.exe`)

- Tiny C++ relay process launched by Chrome when the extension connects
- Bridges Chrome's stdin/stdout native messaging protocol with a Win32 named pipe (`\\.\pipe\lvt_chromium`)
- Supports `--register` to set up Windows registry entries

### Plugin DLL (`lvt_chromium_plugin.dll`)

- Implements the standard lvt plugin interface ([plugin.h](../src/plugin.h))
- Detection: checks for `chrome.dll` or `msedge.dll` loaded in the target process
- Enrichment: connects to the named pipe, optionally sends `listTabs` to select a tab by URL/title, sends a `getDOM` request, and parses the response

## Troubleshooting

### "Cannot connect to browser extension"

- Ensure the extension is loaded and active in Chrome/Edge (`chrome://extensions`)
- Run `lvt_chromium_host.exe --register` to (re-)register the native messaging host
- Check that the extension shows "Service worker: active" in the extensions page
- Try reloading the extension

### Empty DOM tree

- The tab must have finished loading (no spinner in the tab)
- Some pages may block debugger attachment (e.g., `chrome://` pages)
- Check `chrome://extensions` for extension errors

### Debug logging

Set `LVT_DEBUG=1` environment variable for verbose plugin logging:

```powershell
$env:LVT_DEBUG = "1"
lvt --name chrome
```

## Limitations

- Inspects the **active tab** by default; pass `--title` together with `--name`, `--pid`, or `--hwnd` to select a tab by URL/title substring or wildcard pattern
- `chrome://` and `edge://` internal pages cannot be inspected
- The browser extension must be installed and the native host registered
- Shadow DOM content is included when `pierce: true` is used (default)

## Future work

- iframe support (separate DOM walks per frame)
- WebView2 support (Chrome embedded in Win32 apps)
- Lazy loading for very large DOM trees
- Chrome Web Store / Edge Add-ons publication
- Implement the plugin ABI v2 persistent lifetime group. Core already
  acquires a complete `lvt_connection_open`/`lvt_connection_get_tree`/
  `lvt_connection_close` implementation (plus `lvt_plugin_free`) once per
  watch or MCP session, forwards the tab selector on every refresh,
  reconnects dead handles, and drains the optional
  `lvt_connection_poll_events`/`lvt_connection_events_free` pair. Chromium
  currently exports only the v1 one-shot path, so existing behavior remains
  unchanged until it adopts v2.

  A future v2 implementation must keep the existing object/array tree schema:
  core validates node field types and bounds nesting/node counts before
  grafting. Incremental event arrays are also count-bounded and each entry's
  `struct_size` is checked before dereference. Plugin-owned tree and event
  buffers are freed on all success and failure paths.
