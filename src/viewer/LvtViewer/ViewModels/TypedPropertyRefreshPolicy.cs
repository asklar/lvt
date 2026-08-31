using System.Text.Json;

namespace LvtViewer.ViewModels;

public enum TypedPropertyRefreshFailureDisposition
{
    Transient,
    Terminal,
    OwnershipLost,
}

public enum TypedPropertyRefreshAttemptStatus
{
    Applied,
    Retry,
    Terminal,
    OwnershipLost,
}

public readonly record struct TypedPropertyRefreshAttemptResult(
    TypedPropertyRefreshAttemptStatus Status,
    string Error = "")
{
    public static TypedPropertyRefreshAttemptResult Applied() =>
        new(TypedPropertyRefreshAttemptStatus.Applied);

    public static TypedPropertyRefreshAttemptResult Failure(
        string error,
        string errorDisposition = "",
        bool? retryable = null)
    {
        var status = TypedPropertyRefreshPolicy.ClassifyFailure(
            errorDisposition, retryable) switch
        {
            TypedPropertyRefreshFailureDisposition.Terminal =>
                TypedPropertyRefreshAttemptStatus.Terminal,
            TypedPropertyRefreshFailureDisposition.OwnershipLost =>
                TypedPropertyRefreshAttemptStatus.OwnershipLost,
            _ => TypedPropertyRefreshAttemptStatus.Retry,
        };
        return new(status, error);
    }

    public static TypedPropertyRefreshAttemptResult Retry(string error) =>
        new(TypedPropertyRefreshAttemptStatus.Retry, error);

    public static TypedPropertyRefreshAttemptResult OwnershipLost(
        string error) =>
        new(TypedPropertyRefreshAttemptStatus.OwnershipLost, error);
}

public sealed class TypedPropertyRefreshRetryBudget
{
    public int AttemptCount { get; private set; }
    public bool IsStopped { get; private set; }
    public bool CanRetry =>
        AttemptCount < TypedPropertyRefreshPolicy.MaximumAutomaticAttempts;
    public int RetryDelayMs =>
        TypedPropertyRefreshPolicy.RetryDelayMs(AttemptCount);

    public int BeginAttempt() => ++AttemptCount;

    public void RegisterRequest(bool resetBudget)
    {
        if (resetBudget)
            Reset();
    }

    public void Stop()
    {
        IsStopped = true;
    }

    public void Reset()
    {
        AttemptCount = 0;
        IsStopped = false;
    }
}

public static class TypedPropertyRefreshPolicy
{
    public const int MaximumAutomaticAttempts = 5;
    public const int InitialDelayMs = 100;
    public const int MaximumDelayMs = 1000;

    public static int RetryDelayMs(int failedAttempt)
    {
        var multiplier = 1 << Math.Min(failedAttempt, 10);
        return Math.Min(InitialDelayMs * multiplier, MaximumDelayMs);
    }

    public static TypedPropertyRefreshFailureDisposition ClassifyFailure(
        string errorDisposition,
        bool? retryable = null)
    {
        if (string.Equals(
                errorDisposition, "ownershipLost",
                StringComparison.OrdinalIgnoreCase))
        {
            return TypedPropertyRefreshFailureDisposition.OwnershipLost;
        }
        if (string.Equals(
                errorDisposition, "terminal",
                StringComparison.OrdinalIgnoreCase) ||
            retryable == false)
        {
            return TypedPropertyRefreshFailureDisposition.Terminal;
        }
        return TypedPropertyRefreshFailureDisposition.Transient;
    }

    public static async Task<TypedPropertyRefreshAttemptResult>
        RunAttemptAsync(
            Func<Task<TypedPropertyRefreshAttemptResult>> attempt,
            Func<bool> ownershipChanged,
            Action<Exception>? logFailure = null)
    {
        try
        {
            return await attempt();
        }
        catch (OperationCanceledException ex)
        {
            return ownershipChanged()
                ? TypedPropertyRefreshAttemptResult.OwnershipLost(ex.Message)
                : TypedPropertyRefreshAttemptResult.Retry(ex.Message);
        }
        catch (Exception ex)
        {
            logFailure?.Invoke(ex);
            return ownershipChanged()
                ? TypedPropertyRefreshAttemptResult.OwnershipLost(ex.Message)
                : TypedPropertyRefreshAttemptResult.Retry(ex.Message);
        }
    }

    public static bool TryValidateSnapshotPayload(
        JsonElement payload,
        out string error)
    {
        if (payload.ValueKind != JsonValueKind.Object)
        {
            error = "typed property response was not an object";
            return false;
        }
        if (!payload.TryGetProperty("schemaId", out var schemaId) ||
            schemaId.ValueKind != JsonValueKind.String ||
            string.IsNullOrWhiteSpace(schemaId.GetString()))
        {
            error = "typed property response had no schemaId";
            return false;
        }
        if (!payload.TryGetProperty("descriptors", out var descriptors) ||
            descriptors.ValueKind != JsonValueKind.Array)
        {
            error = "typed property response had no descriptors array";
            return false;
        }
        if (!payload.TryGetProperty("values", out var values) ||
            values.ValueKind != JsonValueKind.Array)
        {
            error = "typed property response had no values array";
            return false;
        }
        error = "";
        return true;
    }

    public static bool TryValidateMutationPayload(
        JsonElement payload,
        bool requireCleared,
        out string error)
    {
        if (payload.ValueKind != JsonValueKind.Object ||
            !payload.TryGetProperty("value", out var value) ||
            value.ValueKind != JsonValueKind.String ||
            !payload.TryGetProperty("runtimeType", out var runtimeType) ||
            runtimeType.ValueKind != JsonValueKind.String ||
            !payload.TryGetProperty("source", out var source) ||
            source.ValueKind != JsonValueKind.String ||
            !payload.TryGetProperty("canClear", out var canClear) ||
            canClear.ValueKind is not
                (JsonValueKind.True or JsonValueKind.False) ||
            !payload.TryGetProperty("overridden", out var overridden) ||
            overridden.ValueKind is not
                (JsonValueKind.True or JsonValueKind.False) ||
            (requireCleared &&
             (!payload.TryGetProperty("cleared", out var cleared) ||
              cleared.ValueKind != JsonValueKind.True)))
        {
            error =
                "typed property mutation omitted effective readback metadata";
            return false;
        }

        error = "";
        return true;
    }
}
