namespace LvtViewer.ViewModels;

public sealed class TypedPropertyRefreshState
{
    private long _requestedGeneration;
    private long _appliedGeneration;
    private bool _running;

    public bool HasPending => _requestedGeneration > _appliedGeneration;
    public bool IsRunning => _running;

    public long Request() => ++_requestedGeneration;

    public bool TryBegin(out long generation)
    {
        generation = _requestedGeneration;
        if (_running || !HasPending)
            return false;
        _running = true;
        return true;
    }

    public void Complete(long generation, bool applied)
    {
        if (applied && generation > _appliedGeneration)
            _appliedGeneration = generation;
        _running = false;
    }

    public void Reset()
    {
        _requestedGeneration = 0;
        _appliedGeneration = 0;
        _running = false;
    }
}
