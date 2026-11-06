// Scripts of one extension share a world, so top-level declarations stay inside a block.
{
    // Always one idle callback pending, so one is pending whenever the world is detached.
    const tick = () => {
        const root = document.documentElement;
        root.dataset.idleTicks = String(Number(root.dataset.idleTicks || 0) + 1);
        requestIdleCallback(tick);
    };
    requestIdleCallback(tick);
}
