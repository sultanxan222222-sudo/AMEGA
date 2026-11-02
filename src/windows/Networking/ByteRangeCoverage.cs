namespace AMEGA.Windows.Networking;

// Tracks the union of byte ranges actually read from a single source file.
// Overlapping/retried ranges are counted once, and out-of-file ranges ignored.
internal sealed class ByteRangeCoverage
{
    private readonly object _sync = new();
    private readonly List<(long Start, long End)> _ranges = new();
    private long _coveredBytes;

    public ByteRangeCoverage(long totalBytes)
    {
        if (totalBytes <= 0)
            throw new ArgumentOutOfRangeException(nameof(totalBytes));
        TotalBytes = totalBytes;
    }

    public long TotalBytes { get; }

    public long CoveredBytes
    {
        get
        {
            lock (_sync) return _coveredBytes;
        }
    }

    public void Add(long start, int count)
    {
        if (count <= 0 || start < 0 || start >= TotalBytes) return;

        lock (_sync)
        {
            long mergedStart = start;
            long mergedEnd = Math.Min(TotalBytes, start + (long)count);
            int index = 0;

            while (index < _ranges.Count && _ranges[index].End < mergedStart)
                index++;

            while (index < _ranges.Count && _ranges[index].Start <= mergedEnd)
            {
                mergedStart = Math.Min(mergedStart, _ranges[index].Start);
                mergedEnd = Math.Max(mergedEnd, _ranges[index].End);
                _ranges.RemoveAt(index);
            }

            _ranges.Insert(index, (mergedStart, mergedEnd));

            long covered = 0;
            foreach (var range in _ranges)
                covered += range.End - range.Start;
            _coveredBytes = Math.Min(TotalBytes, covered);
        }
    }
}
