namespace AMEGA.Windows.Networking;

// Wraps a seekable file stream and reports the actual byte intervals read by
// the HTTP response pipeline. Each HTTP request gets its own wrapper.
internal sealed class RangeTrackingReadStream : Stream
{
    private readonly Stream _inner;
    private readonly Action<long, int> _onRead;

    public RangeTrackingReadStream(Stream inner, Action<long, int> onRead)
    {
        _inner = inner ?? throw new ArgumentNullException(nameof(inner));
        _onRead = onRead ?? throw new ArgumentNullException(nameof(onRead));
        if (!inner.CanRead || !inner.CanSeek)
            throw new ArgumentException("The wrapped stream must be readable and seekable.", nameof(inner));
    }

    public override bool CanRead => _inner.CanRead;
    public override bool CanSeek => _inner.CanSeek;
    public override bool CanWrite => false;
    public override long Length => _inner.Length;

    public override long Position
    {
        get => _inner.Position;
        set => _inner.Position = value;
    }

    public override void Flush() => _inner.Flush();

    public override int Read(byte[] buffer, int offset, int count)
    {
        long start = _inner.Position;
        int read = _inner.Read(buffer, offset, count);
        if (read > 0) _onRead(start, read);
        return read;
    }

    public override int Read(Span<byte> buffer)
    {
        long start = _inner.Position;
        int read = _inner.Read(buffer);
        if (read > 0) _onRead(start, read);
        return read;
    }

    public override async Task<int> ReadAsync(
        byte[] buffer, int offset, int count, CancellationToken cancellationToken)
    {
        long start = _inner.Position;
        int read = await _inner.ReadAsync(buffer.AsMemory(offset, count), cancellationToken)
            .ConfigureAwait(false);
        if (read > 0) _onRead(start, read);
        return read;
    }

    public override async ValueTask<int> ReadAsync(
        Memory<byte> buffer, CancellationToken cancellationToken = default)
    {
        long start = _inner.Position;
        int read = await _inner.ReadAsync(buffer, cancellationToken).ConfigureAwait(false);
        if (read > 0) _onRead(start, read);
        return read;
    }

    public override long Seek(long offset, SeekOrigin origin) => _inner.Seek(offset, origin);

    public override void SetLength(long value) =>
        throw new NotSupportedException("Read-only stream.");

    public override void Write(byte[] buffer, int offset, int count) =>
        throw new NotSupportedException("Read-only stream.");

    protected override void Dispose(bool disposing)
    {
        if (disposing) _inner.Dispose();
        base.Dispose(disposing);
    }

    public override async ValueTask DisposeAsync()
    {
        await _inner.DisposeAsync().ConfigureAwait(false);
        GC.SuppressFinalize(this);
    }
}
