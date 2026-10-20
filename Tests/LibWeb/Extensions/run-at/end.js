// Scripts of one extension share a world, so top-level declarations stay inside a block.
{
    const root = document.documentElement;
    root.dataset.end = `readyState ${document.readyState}, after start ${root.dataset.start !== undefined}`;
}
