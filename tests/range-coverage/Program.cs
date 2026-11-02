using AMEGA.Windows.Networking;

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

var coverage = new ByteRangeCoverage(100);
coverage.Add(10, 20);
Require(coverage.CoveredBytes == 20, "first interval should cover 20 bytes");
coverage.Add(20, 20);
Require(coverage.CoveredBytes == 30, "overlapping interval must count shared bytes once");
coverage.Add(80, 40);
Require(coverage.CoveredBytes == 50, "range must clamp at file end");
coverage.Add(40, 40);
Require(coverage.CoveredBytes == 90, "adjacent ranges should merge without inventing missing bytes");
coverage.Add(0, 10);
Require(coverage.CoveredBytes == 100, "the final missing interval should complete full coverage");
coverage.Add(-5, 20);
coverage.Add(150, 10);
coverage.Add(10, 0);
Require(coverage.CoveredBytes == 100, "invalid and empty intervals must be ignored");

byte[] payload = Enumerable.Range(0, 100).Select(value => (byte)value).ToArray();
var observed = new List<(long Start, int Count)>();
using (var stream = new RangeTrackingReadStream(
    new MemoryStream(payload),
    (start, count) => observed.Add((start, count))))
{
    stream.Position = 10;
    byte[] first = new byte[10];
    int firstRead = await stream.ReadAsync(first, 0, first.Length, CancellationToken.None);
    Require(firstRead == 10 && first.SequenceEqual(payload.Skip(10).Take(10)),
        "array ReadAsync must preserve bytes and length");

    stream.Position = 30;
    byte[] second = new byte[5];
    int secondRead = stream.Read(second, 0, second.Length);
    Require(secondRead == 5 && second.SequenceEqual(payload.Skip(30).Take(5)),
        "Read must preserve bytes and length");

    stream.Position = 70;
    byte[] third = new byte[8];
    int thirdRead = await stream.ReadAsync(third.AsMemory());
    Require(thirdRead == 8 && third.SequenceEqual(payload.Skip(70).Take(8)),
        "memory ReadAsync must preserve bytes and length");
}

Require(observed.Count == 3, "all supported read overloads must report one interval");
Require(observed[0] == (10, 10), "first reported range must use the true stream offset");
Require(observed[1] == (30, 5), "second reported range must use the true stream offset");
Require(observed[2] == (70, 8), "third reported range must use the true stream offset");

var rpiBody = "{ \"status\": \"success\", \"error\": 0, \"length_total\": 0x100000000, \"transferred_total\": 0x80000000, \"preparing_percent\": 100, \"local_copy_percent\": 42, \"rest_sec_total\": 17 }";
Require(RpiTaskProgressParser.TryParse(rpiBody, out RpiTaskProgress? rpi) && rpi is not null,
    "RPI progress parser must accept its hexadecimal integer response");
Require(rpi!.TotalBytes == 4_294_967_296L && rpi.TransferredBytes == 2_147_483_648L,
    "RPI progress parser must decode large hexadecimal byte counts");
Require(rpi.LocalCopyPercent == 42 && rpi.RemainingSeconds == 17 && rpi.ErrorCode == 0,
    "RPI progress parser must preserve phase, ETA and error fields");
Require(!RpiTaskProgressParser.TryParse("{ \"status\": \"fail\", \"error_code\": 0x80990085 }", out _),
    "RPI error responses must not be mistaken for successful task progress");

Console.WriteLine("AMEGA HTTP Range coverage and stream tests passed");
