// Stands in for the library the cleanup script uses; it has to stay in the extension's world for the next one.
window.bpcLibrary = {
    removeAll: (document, selector) => document.querySelectorAll(selector).forEach(element => element.remove()),
};
