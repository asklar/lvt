# Chromium Plugin — Chrome/Edge DOM Inspection

The Chromium plugin lets lvt inspect the DOM tree of web pages in Google Chrome and Microsoft Edge. It works via a browser extension that communicates with lvt through Chrome's [Native Messaging](https://developer.chrome.com/docs/extensions/develop/concepts/native-messaging) protocol.

## How it works

```
lvt.exe → plugin DLL → named pipe → native host → Chrome extension → chrome.debugger (CDP) → DOM
```

1. **lvt** detects Chrome/Edge by checking for `chrome.dll` or `msedge.dll` in the target process
2. The **plugin** connects to a named pipe served by the native messaging host
3. The **native messaging host** relays the request to the browser extension
4. The **extension** uses the `chrome.debugger` API (Chrome DevTools Protocol) to walk the DOM tree of the active tab
5. The DOM tree is returned as an lvt element tree with bounds, properties, and text content

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

The extension icon should appear in the toolbar. The extension will automatically connect to the native messaging host.

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
```

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

- **Service worker** (`service-worker.js`): Connects to the native messaging host, dispatches DOM requests, uses `chrome.debugger` API for DOM walking
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
- Enrichment: connects to the named pipe, sends a `getDOM` request, and parses the response

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

- Only inspects the **active tab** (tab selection by URL/title is planned)
- `chrome://` and `edge://` internal pages cannot be inspected
- The browser extension must be installed and the native host registered
- Shadow DOM content is included when `pierce: true` is used (default)

## Future work

- Tab selection by URL or title pattern
- iframe support (separate DOM walks per frame)
- WebView2 support (Chrome embedded in Win32 apps)
- Lazy loading for very large DOM trees
- Chrome Web Store / Edge Add-ons publication
