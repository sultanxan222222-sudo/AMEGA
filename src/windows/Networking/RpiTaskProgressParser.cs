using System.Globalization;
using System.Text.RegularExpressions;

namespace AMEGA.Windows.Networking;

internal sealed record RpiTaskProgress(
    long TotalBytes,
    long TransferredBytes,
    int PreparingPercent,
    int LocalCopyPercent,
    long RemainingSeconds,
    int ErrorCode);

internal static class RpiTaskProgressParser
{
    public static bool TryParse(string body, out RpiTaskProgress? progress)
    {
        progress = null;
        if (!TryReadString(body, "status", out string status) ||
            !status.Equals("success", StringComparison.OrdinalIgnoreCase))
            return false;

        if (!TryReadInteger(body, "length_total", out long total) ||
            !TryReadInteger(body, "transferred_total", out long transferred))
            return false;

        TryReadInteger(body, "preparing_percent", out long preparing);
        TryReadInteger(body, "local_copy_percent", out long localCopy);
        TryReadInteger(body, "rest_sec_total", out long remaining);
        TryReadInteger(body, "error", out long error);

        progress = new RpiTaskProgress(
            Math.Max(0, total),
            Math.Max(0, transferred),
            (int)Math.Clamp(preparing, 0, 100),
            (int)Math.Clamp(localCopy, 0, 100),
            Math.Max(0, remaining),
            (int)Math.Clamp(error, int.MinValue, int.MaxValue));
        return true;
    }

    private static bool TryReadString(string body, string name, out string value)
    {
        string quote = ((char)34).ToString();
        string pattern = quote + Regex.Escape(name) + quote + @"\s*:\s*" + quote + @"(?<value>[^" + quote + @"]*)" + quote;
        Match match = Regex.Match(body, pattern, RegexOptions.CultureInvariant);
        value = match.Success ? match.Groups["value"].Value : string.Empty;
        return match.Success;
    }

    private static bool TryReadInteger(string body, string name, out long value)
    {
        string quote = ((char)34).ToString();
        string pattern = quote + Regex.Escape(name) + quote + @"\s*:\s*(?<value>0[xX][0-9a-fA-F]+|-?[0-9]+)";
        Match match = Regex.Match(body, pattern, RegexOptions.CultureInvariant);
        if (!match.Success)
        {
            value = 0;
            return false;
        }

        string raw = match.Groups["value"].Value;
        if (raw.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            return long.TryParse(raw.AsSpan(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out value);
        return long.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture, out value);
    }
}
