// Scripts of one extension share a world, so top-level declarations stay inside a block.
{
    // Ad blockers inject their scriptlets this way at document_start: a script element the page runs, removed again.
    const script = document.createElement("script");
    script.textContent = "document.documentElement.dataset.appendedScript = 'ran';";
    document.documentElement.appendChild(script);
    script.remove();
}
