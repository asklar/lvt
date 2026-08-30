namespace LvtViewer.ViewModels;

public sealed class TypedPropertyRefreshState
{
    public readonly record struct Token(long Epoch, long Generation);

    private long _epoch = 1;
    private long _requestedGeneration;
    private long _appliedGeneration;
    private Token? _active;

    public bool HasPending => _requestedGeneration > _appliedGeneration;
    public bool IsRunning => _active.HasValue;
    public long Epoch => _epoch;
    public bool IsCurrent(Token token) =>
        token.Epoch == _epoch &&
        token.Generation == _requestedGeneration &&
        _active == token;

    public Token Request() =>
        new(_epoch, ++_requestedGeneration);

    public bool TryBegin(out Token token)
    {
        token = new Token(_epoch, _requestedGeneration);
        if (_active.HasValue || !HasPending)
            return false;
        _active = token;
        return true;
    }

    public bool Complete(Token token, bool applied)
    {
        if (_active != token || token.Epoch != _epoch)
            return false;
        if (applied && token.Generation > _appliedGeneration)
            _appliedGeneration = token.Generation;
        _active = null;
        return true;
    }

    public void Reset()
    {
        ++_epoch;
        _requestedGeneration = 0;
        _appliedGeneration = 0;
        _active = null;
    }
}
