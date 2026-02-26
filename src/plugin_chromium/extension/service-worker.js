// LVT Chromium Extension — Service Worker
// Connects to the lvt native messaging host and dispatches DOM tree requests.

const NATIVE_HOST_NAME = "com.lvt.chromium";

let nativePort = null;

function connectToHost() {
  try {
    nativePort = chrome.runtime.connectNative(NATIVE_HOST_NAME);
    console.log("LVT: Connected to native messaging host");

    nativePort.onMessage.addListener(handleHostMessage);
    nativePort.onDisconnect.addListener(() => {
      console.log("LVT: Native host disconnected", chrome.runtime.lastError?.message);
      nativePort = null;
      // Reconnect after a delay
      setTimeout(connectToHost, 5000);
    });
  } catch (e) {
    console.error("LVT: Failed to connect to native host:", e);
    setTimeout(connectToHost, 5000);
  }
}

async function handleHostMessage(message) {
  console.log("LVT: Received message from host:", message.type);

  if (message.type === "getDOM") {
    try {
      const result = await getActiveTabDOM(message);
      nativePort?.postMessage(result);
    } catch (e) {
      nativePort?.postMessage({
        type: "error",
        requestId: message.requestId,
        message: e.message || String(e)
      });
    }
  } else if (message.type === "ping") {
    nativePort?.postMessage({ type: "pong" });
  }
}

async function getActiveTabDOM(request) {
  // Find the active tab in the last focused window
  let tabs = await chrome.tabs.query({ active: true, lastFocusedWindow: true });

  // Filter out non-debuggable tabs (chrome://, edge://, about:, etc.)
  tabs = tabs.filter(t => t.url && !t.url.startsWith("chrome://") &&
    !t.url.startsWith("edge://") && !t.url.startsWith("about:") &&
    !t.url.startsWith("chrome-extension://"));

  // If no debuggable active tab, try any active tab across all windows
  if (!tabs.length) {
    tabs = await chrome.tabs.query({ active: true });
    tabs = tabs.filter(t => t.url && !t.url.startsWith("chrome://") &&
      !t.url.startsWith("edge://") && !t.url.startsWith("about:") &&
      !t.url.startsWith("chrome-extension://"));
  }

  if (!tabs.length) {
    throw new Error("No debuggable tab found (chrome:// and edge:// pages cannot be inspected)");
  }

  const tab = tabs[0];
  if (!tab.id) {
    throw new Error("Active tab has no ID");
  }

  // Attach the debugger to the tab
  const target = { tabId: tab.id };

  try {
    await chrome.debugger.attach(target, "1.3");
  } catch (e) {
    // May already be attached
    if (!e.message?.includes("Already attached")) {
      throw new Error(`Failed to attach debugger: ${e.message}`);
    }
  }

  try {
    // Enable DOM domain
    await chrome.debugger.sendCommand(target, "DOM.enable");

    // Get the full DOM tree
    const docResult = await chrome.debugger.sendCommand(target, "DOM.getDocument", {
      depth: -1,
      pierce: true
    });

    // Get layout metrics for bounding boxes
    const tree = await convertDOMTree(target, docResult.root);

    return {
      type: "domTree",
      requestId: request.requestId,
      url: tab.url || "",
      title: tab.title || "",
      tree: tree ? [tree] : []
    };
  } finally {
    try {
      await chrome.debugger.detach(target);
    } catch (e) {
      // Ignore detach errors
    }
  }
}

// Convert a CDP DOM.Node tree to lvt element format
async function convertDOMTree(target, node, depth = 0) {
  // Skip non-element nodes (except document/document fragment)
  const ELEMENT_NODE = 1;
  const DOCUMENT_NODE = 9;
  const DOCUMENT_FRAGMENT_NODE = 11;

  if (node.nodeType !== ELEMENT_NODE &&
      node.nodeType !== DOCUMENT_NODE &&
      node.nodeType !== DOCUMENT_FRAGMENT_NODE) {
    return null;
  }

  const element = {
    type: node.nodeName || "",
  };

  // Parse attributes into properties
  if (node.attributes && node.attributes.length > 0) {
    const props = {};
    for (let i = 0; i < node.attributes.length; i += 2) {
      props[node.attributes[i]] = node.attributes[i + 1] || "";
    }
    element.properties = props;
  }

  // Get text content for leaf elements
  if (node.children) {
    const textChildren = node.children.filter(c => c.nodeType === 3 /* TEXT_NODE */);
    const text = textChildren.map(c => c.nodeValue?.trim()).filter(Boolean).join(" ");
    if (text) {
      element.text = text;
    }
  }

  // Try to get bounding box (only for element nodes, skip deep trees for perf)
  if (node.nodeType === ELEMENT_NODE && depth < 50) {
    try {
      const boxModel = await chrome.debugger.sendCommand(target, "DOM.getBoxModel", {
        nodeId: node.nodeId
      });
      if (boxModel?.model?.content) {
        const quad = boxModel.model.content;
        // quad is [x1,y1, x2,y2, x3,y3, x4,y4] — use top-left and dimensions
        element.offsetX = Math.round(quad[0]);
        element.offsetY = Math.round(quad[1]);
        element.width = Math.round(quad[2] - quad[0]);
        element.height = Math.round(quad[5] - quad[1]);
      }
    } catch (e) {
      // getBoxModel fails for invisible elements — that's fine
    }
  }

  // Recurse into children
  const allChildren = [
    ...(node.children || []),
    ...(node.shadowRoots || []),
    ...(node.contentDocument ? [node.contentDocument] : [])
  ];

  if (allChildren.length > 0) {
    const children = [];
    for (const child of allChildren) {
      const converted = await convertDOMTree(target, child, depth + 1);
      if (converted) {
        children.push(converted);
      }
    }
    if (children.length > 0) {
      element.children = children;
    }
  }

  return element;
}

// Connect on startup and on install/update
connectToHost();

chrome.runtime.onInstalled.addListener(() => {
  console.log("LVT: Extension installed/updated");
  if (!nativePort) connectToHost();
});
