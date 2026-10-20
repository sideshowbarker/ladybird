// Scripts of one extension share a world, so top-level declarations stay inside a block.
{
    const root = document.documentElement;
    root.dataset.startCount = String(Number(root.dataset.startCount || 0) + 1);
    root.dataset.start = `body ${document.body === null ? "absent" : "present"}, readyState ${document.readyState}`;
}
