namespace AMEGA.Windows.Networking;

internal sealed record DiscoveredReceiver(
    string ReceiverId,
    string DisplayName,
    string IpAddress,
    string Version,
    string InstanceNonce,
    DateTimeOffset LastSeen)
{
    public string DisplayLabel => $"{DisplayName} — {IpAddress} — v{Version}";
}
