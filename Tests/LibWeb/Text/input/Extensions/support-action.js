// Shared by the action tests: calls a method of the extension's action API from its background page.
function callAction(method, ...args) {
    return sendToBackground({ type: "call", method, args });
}

// An extension URL's path, without the extension's id.
function extensionPath(result) {
    if (typeof result.value === "string") result.value = result.value.replace(/^ladybird-extension:\/\/[^/]+/, "");
    return JSON.stringify(result);
}
