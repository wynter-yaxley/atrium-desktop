using System.Text.Json.Nodes;

namespace Atrium.Settings;

// Pure state machine for the Settings window's saved baseline, editable draft,
// optimistic concurrency metadata (InstanceId / Revision), and 3-way merges.
internal sealed class SettingsSession
{
    public const int MaxPins = 64;

    public BarSettings Saved { get; private set; } = new();
    public BarSettings Draft { get; private set; } = new();
    public string? InstanceId { get; private set; }
    public ulong Revision { get; private set; }
    public long DraftVersion { get; private set; }
    public bool Initialized { get; private set; }
    public bool Closed { get; private set; }

    public bool HasUnsavedChanges => !Draft.SameAs(Saved);
    public bool HasPinOverflow => Draft.Pins is { Count: > MaxPins };

    public IReadOnlyList<PinnedApp> OverflowPins
    {
        get
        {
            if (Draft.Pins is not { Count: > MaxPins } draftPins) return [];
            int excess = draftPins.Count - MaxPins;
            var savedKeys = new HashSet<PinKey>(PinKeyComparer.Instance);
            if (Saved.Pins != null)
            {
                foreach (var pin in Saved.Pins) savedKeys.Add(KeyOf(pin));
            }
            var unsaved = new List<PinnedApp>();
            foreach (var pin in draftPins)
            {
                if (!savedKeys.Contains(KeyOf(pin))) unsaved.Add(pin);
            }
            return unsaved.Count >= excess
                ? unsaved.GetRange(unsaved.Count - excess, excess)
                : draftPins.GetRange(MaxPins, excess);
        }
    }

    public void MarkEdited()
    {
        if (Closed) return;
        DraftVersion++;
    }

    public void Revert()
    {
        if (Closed) return;
        Draft = Saved.Clone();
        DraftVersion++;
    }

    // A settings file from another computer replaces the draft. The saved baseline and the revision
    // stay, so the window shows unsaved changes that a save then commits.
    public void Import(BarSettings imported)
    {
        if (Closed) return;
        Draft = imported.Clone();
        DraftVersion++;
    }

    public void Close() => Closed = true;

    public bool ApplyInitialState(
        BarSettings saved,
        BarSettings current,
        string? instanceId,
        ulong revision,
        BarSettings? pendingSaved = null,
        BarSettings? pendingCurrent = null)
    {
        if (Closed) return false;
        Initialized = true;
        Saved = saved.Clone();
        InstanceId = instanceId;
        Revision = revision;
        if (pendingSaved != null && pendingCurrent != null)
        {
            Draft = MergeThreeWay(pendingSaved, saved, pendingCurrent);
            if (!Draft.SameAs(Saved)) DraftVersion++;
        }
        else
        {
            Draft = current.Clone();
        }
        return HasUnsavedChanges;
    }

    public bool ApplyReconnectSnapshot(
        BarSettings saved,
        BarSettings current,
        string? instanceId,
        ulong revision,
        long draftVersionBeforeAwait)
    {
        if (Closed) return false;
        Initialized = true;
        InstanceId = instanceId;
        Revision = revision;
        bool keepDraft = DraftVersion != draftVersionBeforeAwait || !Draft.SameAs(Saved);
        Draft = keepDraft ? MergeThreeWay(Saved, saved, Draft) : current.Clone();
        Saved = saved.Clone();
        return HasUnsavedChanges;
    }

    public bool ApplyCommitSuccess(BarSettings sent, string? instanceId, ulong revision)
    {
        if (Closed) return false;
        Saved = sent.Clone();
        if (instanceId != null) InstanceId = instanceId;
        if (revision > 0) Revision = revision;
        return true;
    }

    public bool ApplyExternalChange(BarSettings newSaved, ulong revision = 0)
    {
        if (Closed) return false;
        if (revision > 0) Revision = revision;
        if (newSaved.SameAs(Saved)) return false;
        Draft = MergeThreeWay(Saved, newSaved, Draft);
        Saved = newSaved.Clone();
        return true;
    }

    public bool ApplyConflict(BarSettings serverSaved, ulong serverRevision, string? serverInstanceId)
    {
        if (Closed) return false;
        Draft = MergeThreeWay(Saved, serverSaved, Draft);
        Saved = serverSaved.Clone();
        Revision = serverRevision;
        if (serverInstanceId != null) InstanceId = serverInstanceId;
        DraftVersion++;
        return true;
    }

    private const string AsciiWs = " \t\r\n\v\f";

    private readonly record struct PinKey(string Path, string Aumid, string Args);

    private static PinKey KeyOf(PinnedApp p) => new(p.Path, p.Aumid, p.Args);

    private sealed class PinKeyComparer : IEqualityComparer<PinKey>
    {
        public static readonly PinKeyComparer Instance = new();

        public bool Equals(PinKey x, PinKey y) =>
            string.Equals(x.Path, y.Path, StringComparison.OrdinalIgnoreCase) &&
            string.Equals(x.Aumid, y.Aumid, StringComparison.OrdinalIgnoreCase) &&
            string.Equals(x.Args, y.Args, StringComparison.Ordinal);

        public int GetHashCode(PinKey obj) => HashCode.Combine(
            StringComparer.OrdinalIgnoreCase.GetHashCode(obj.Path),
            StringComparer.OrdinalIgnoreCase.GetHashCode(obj.Aumid),
            StringComparer.Ordinal.GetHashCode(obj.Args));
    }

    private static BarSettings MergeThreeWay(BarSettings baseSaved, BarSettings targetSaved, BarSettings draft)
    {
        var baseJson = baseSaved.ToJson();
        var targetJson = targetSaved.ToJson();
        var draftJson = draft.ToJson();
        var merged = new JsonObject();
        var keys = new HashSet<string>(baseJson.Select(kv => kv.Key), StringComparer.Ordinal);
        foreach (var kv in targetJson) keys.Add(kv.Key);
        foreach (var kv in draftJson) keys.Add(kv.Key);
        foreach (string key in keys)
        {
            if (key is "pins" or "stageExcluded" or "nowPlayingApps" or "folderStacks" or "folderStackFiles" or "controlModules") continue;
            baseJson.TryGetPropertyValue(key, out var baseVal);
            draftJson.TryGetPropertyValue(key, out var draftVal);
            bool draftChanged = !JsonNode.DeepEquals(draftVal, baseVal);
            var chosen = draftChanged ? draftVal : (targetJson.TryGetPropertyValue(key, out var targetVal) ? targetVal : null);
            if (chosen != null)
                merged[key] = chosen.DeepClone();
        }
        var result = BarSettings.FromJson(merged, new());
        result.Pins = MergePins(baseSaved.Pins, targetSaved.Pins, draft.Pins);
        result.StageExcluded = MergeStringList(baseSaved.StageExcluded, targetSaved.StageExcluded, draft.StageExcluded, TrimAsciiKey);
        result.NowPlayingApps = MergeStringList(baseSaved.NowPlayingApps, targetSaved.NowPlayingApps, draft.NowPlayingApps, TrimAsciiKey, 64);
        result.FolderStacks = MergeStringList(baseSaved.FolderStacks, targetSaved.FolderStacks, draft.FolderStacks, NormalizeFolderKey, 32);
        result.FolderStackFiles = MergeStringList(
            baseSaved.FolderStackFiles, targetSaved.FolderStackFiles, draft.FolderStackFiles, NormalizeFileKey, 32);
        result.ControlModules = MergeStringList(
            NormalizeControlModules(baseSaved.ControlModules),
            NormalizeControlModules(targetSaved.ControlModules),
            NormalizeControlModules(draft.ControlModules),
            TrimAsciiKey,
            BarSettings.ControlModuleIds.Length);
        return result;
    }

    private static bool PinsListEqual(List<PinnedApp>? a, List<PinnedApp>? b) =>
        ReferenceEquals(a, b) || (a != null && b != null && a.SequenceEqual(b));

    private static List<PinnedApp>? MergePins(List<PinnedApp>? basePins, List<PinnedApp>? targetPins, List<PinnedApp>? draftPins)
    {
        List<PinnedApp>? merged;
        if (PinsListEqual(draftPins, basePins))
            merged = targetPins == null ? null : [.. targetPins];
        else if (PinsListEqual(targetPins, basePins))
            merged = draftPins == null ? null : [.. draftPins];
        else if (draftPins == null)
            merged = targetPins == null ? null : [.. targetPins];
        else if (targetPins == null)
            merged = [.. draftPins];
        else if (basePins == null)
            merged = draftPins.Count == 0 ? [] : MergePinLists([], targetPins, draftPins);
        else
            merged = MergePinLists(basePins, targetPins, draftPins);
        return merged;
    }

    private static List<PinnedApp> MergePinLists(List<PinnedApp> basePins, List<PinnedApp> targetPins, List<PinnedApp> draftPins) =>
        MergeThreeWayList(
            basePins, targetPins, draftPins,
            KeyOf,
            PinKeyComparer.Instance,
            static (b, t, d) => b == null ? d : new PinnedApp(
                d.Name != b.Name ? d.Name : t.Name,
                d.Path != b.Path ? d.Path : t.Path,
                d.Args != b.Args ? d.Args : t.Args,
                d.Aumid != b.Aumid ? d.Aumid : t.Aumid,
                d.MultipleWindows != b.MultipleWindows ? d.MultipleWindows : t.MultipleWindows));

    private static string TrimAsciiKey(string s) => s.AsSpan().Trim(AsciiWs).ToString();

    private static string NormalizeFileKey(string s) => s.AsSpan().Trim(AsciiWs).ToString().Replace('/', '\\');

    private static List<string> NormalizeControlModules(IEnumerable<string> list) =>
        [.. list.Select(id => TrimAsciiKey(id) is "nightlight" ? "darkmode" : TrimAsciiKey(id))
                .Where(BarSettings.ControlModuleIds.Contains)
                .Distinct(StringComparer.Ordinal)
                .Take(BarSettings.ControlModuleIds.Length)];

    private static bool IsFolderRootSlash(ReadOnlySpan<char> path)
    {
        if (path.Length == 1) return path[0] is '\\' or '/';
        if (path.Length == 2) return path[0] == '\\' && path[1] == '\\';
        if (path.Length == 3) return char.IsAsciiLetter(path[0]) && path[1] == ':' && path[2] is '\\' or '/';
        if (path.Length == 7 && (path.StartsWith(@"\\?\") || path.StartsWith(@"\\.\")))
            return char.IsAsciiLetter(path[4]) && path[5] == ':' && path[6] is '\\' or '/';
        return false;
    }

    private static string NormalizeFolderKey(string s)
    {
        var span = s.AsSpan().Trim(AsciiWs);
        while (span.Length > 0 && span[^1] is '\\' or '/' && !IsFolderRootSlash(span))
            span = span[..^1];
        return span.ToString().Replace('/', '\\');
    }

    private static List<string> MergeStringList(
        List<string> baseList, List<string> targetList, List<string> draftList, Func<string, string> keyOf, int maxCount = int.MaxValue)
    {
        if (draftList.SequenceEqual(baseList, StringComparer.Ordinal))
            return targetList.Count <= maxCount ? [.. targetList] : [.. targetList.Take(maxCount)];
        if (targetList.SequenceEqual(baseList, StringComparer.Ordinal))
            return draftList.Count <= maxCount ? [.. draftList] : [.. draftList.Take(maxCount)];
        var merged = MergeThreeWayList(
            baseList, targetList, draftList,
            keyOf,
            StringComparer.OrdinalIgnoreCase,
            static (b, t, d) => b == null || d != b ? d : t);
        if (merged.Count > maxCount)
            merged.RemoveRange(maxCount, merged.Count - maxCount);
        return merged;
    }

    private static List<T> MergeThreeWayList<T, TKey>(
        IReadOnlyList<T> baseList,
        IReadOnlyList<T> targetList,
        IReadOnlyList<T> draftList,
        Func<T, TKey> keyOf,
        IEqualityComparer<TKey> comparer,
        Func<T?, T, T, T> mergeItem) where T : class where TKey : notnull
    {
        static (List<TKey> Keys, Dictionary<TKey, T> Map) Dedupe(IReadOnlyList<T> items, Func<T, TKey> keyOf, IEqualityComparer<TKey> cmp)
        {
            var keys = new List<TKey>(items.Count);
            var map = new Dictionary<TKey, T>(items.Count, cmp);
            foreach (var item in items)
            {
                var k = keyOf(item);
                if (map.TryAdd(k, item)) keys.Add(k);
            }
            return (keys, map);
        }

        var (baseKeys, baseMap) = Dedupe(baseList, keyOf, comparer);
        var (targetKeys, targetMap) = Dedupe(targetList, keyOf, comparer);
        var (draftKeys, draftMap) = Dedupe(draftList, keyOf, comparer);

        var survived = new HashSet<TKey>(comparer);
        foreach (var k in baseKeys)
            if (targetMap.ContainsKey(k) && draftMap.ContainsKey(k)) survived.Add(k);
        foreach (var k in targetKeys)
            if (!baseMap.ContainsKey(k)) survived.Add(k);
        foreach (var k in draftKeys)
            if (!baseMap.ContainsKey(k)) survived.Add(k);

        var baseS = baseKeys.Where(survived.Contains).ToList();
        var draftS = draftKeys.Where(k => baseMap.ContainsKey(k) && survived.Contains(k)).ToList();
        bool draftReordered = !draftS.SequenceEqual(baseS, comparer);
        var primaryKeys = draftReordered ? draftKeys : targetKeys;
        var secondaryKeys = draftReordered ? targetKeys : draftKeys;

        var resultKeys = new List<TKey>();
        var primarySet = new HashSet<TKey>(comparer);
        foreach (var k in primaryKeys)
        {
            if (survived.Contains(k)) resultKeys.Add(k);
            primarySet.Add(k);
        }

        var secSurvived = secondaryKeys.Where(survived.Contains).ToList();
        for (int i = 0; i < secSurvived.Count;)
        {
            var k = secSurvived[i];
            if (baseMap.ContainsKey(k) || primarySet.Contains(k)) { i++; continue; }
            int start = i;
            while (i < secSurvived.Count && !baseMap.ContainsKey(secSurvived[i]) && !primarySet.Contains(secSurvived[i])) i++;
            var run = secSurvived.GetRange(start, i - start);
            if (i >= secSurvived.Count) { resultKeys.AddRange(run); }
            else if (start == 0) { resultKeys.InsertRange(0, run); }
            else
            {
                var prev = secSurvived[start - 1];
                int anchor = resultKeys.FindIndex(x => comparer.Equals(x, prev));
                if (anchor >= 0) resultKeys.InsertRange(anchor + 1, run);
                else resultKeys.AddRange(run);
            }
        }

        var result = new List<T>(resultKeys.Count);
        foreach (var k in resultKeys)
        {
            if (baseMap.TryGetValue(k, out var b))
                result.Add(mergeItem(b, targetMap[k], draftMap[k]));
            else if (draftMap.TryGetValue(k, out var d))
                result.Add(targetMap.TryGetValue(k, out var t) ? mergeItem(null, t, d) : d);
            else
                result.Add(targetMap[k]);
        }
        return result;
    }
}
