// Flat undo/redo command stack for the Generator (Phase 4 lane edits).
//
// Snapshot-based: callers push a deep-cloned snapshot of the lane model BEFORE
// each mutating edit. undo() restores the previous snapshot; redo() re-applies.
// Snapshots are plain JSON-serializable objects (the lane model is), so a
// structuredClone-free JSON deep clone is exact and cheap for these small docs.
//
// createUndoStack({ get, set }):
//   get() -> current state (must be JSON-serializable)
//   set(state) -> install a restored state (re-renders the view)

function clone(x) { return x === undefined ? undefined : JSON.parse(JSON.stringify(x)); }

export function createUndoStack(ctx, opts) {
    opts = opts || {};
    const limit = opts.limit || 100;
    const undoStack = [];
    const redoStack = [];

    // Call BEFORE mutating: captures the current state as an undo point.
    function push() {
        undoStack.push(clone(ctx.get()));
        if (undoStack.length > limit) undoStack.shift();
        redoStack.length = 0;
    }

    function canUndo() { return undoStack.length > 0; }
    function canRedo() { return redoStack.length > 0; }

    function undo() {
        if (!undoStack.length) return false;
        redoStack.push(clone(ctx.get()));
        ctx.set(undoStack.pop());
        return true;
    }

    function redo() {
        if (!redoStack.length) return false;
        undoStack.push(clone(ctx.get()));
        ctx.set(redoStack.pop());
        return true;
    }

    function clear() { undoStack.length = 0; redoStack.length = 0; }

    // Keyboard handler: Ctrl/Cmd-Z = undo, Shift+Ctrl/Cmd-Z (or Ctrl-Y) = redo.
    // Returns true when it handled the event (so the caller can preventDefault).
    function handleKey(e) {
        const mod = e.ctrlKey || e.metaKey;
        if (!mod) return false;
        const k = (e.key || '').toLowerCase();
        if (k === 'z') {
            if (e.shiftKey) { return redo(); }
            return undo();
        }
        if (k === 'y') { return redo(); }
        return false;
    }

    return { push, undo, redo, clear, canUndo, canRedo, handleKey };
}
