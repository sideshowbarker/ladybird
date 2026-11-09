// Firefox hands a content script the page's own window as self.wrappedJSObject, so it can read what the page's scripts
// put on their global; the world's own globals stay apart from the page's.
{
    const page = self.wrappedJSObject;
    const script = document.createElement("script");
    script.textContent =
        "window.pageMarker = 'set by the page'; window.pageHasWrapper = typeof window.wrappedJSObject;";
    document.documentElement.appendChild(script);
    script.remove();
    self.worldMarker = "set by the world";
    document.documentElement.dataset.wrappedJsObject = JSON.stringify({
        type: typeof page,
        pageMarker: page && page.pageMarker,
        sameObjectEachTime: self.wrappedJSObject === page && window.wrappedJSObject === page,
        worldMarkerOnPage: page && page.worldMarker,
        pageMarkerInWorld: self.pageMarker,
        pageHasWrapper: page && page.pageHasWrapper,
    });
}
