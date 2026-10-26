const output = [];
try {
    const get = browser.i18n.getMessage;
    const style = getComputedStyle(document.getElementById("target"));
    output.push(`content script id: ${get("@@extension_id") === browser.runtime.id}`);
    output.push(`CSS id URL: ${style.getPropertyValue("--extension-url").includes(browser.runtime.getURL("img.png"))}`);
    output.push(`CSS direction: ${style.direction === get("@@bidi_dir")}`);
    output.push(`CSS edge: ${style.textAlign === get("@@bidi_start_edge")}`);
} catch (error) {
    output.push(`ERROR: ${error.name}: ${error.message}`);
}
document.documentElement.dataset.i18nOutput = JSON.stringify(output);
document.dispatchEvent(new Event("i18n-finished"));
