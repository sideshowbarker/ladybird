// Scripts of one extension share a world, so top-level declarations stay inside a block.
{
    const root = document.documentElement;
    root.dataset.idle = `readyState ${document.readyState}, after end ${root.dataset.end !== undefined}`;
    document.dispatchEvent(new CustomEvent("content-scripts-idle"));
}
