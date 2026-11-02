namespace AMEGA.Windows.Networking;

internal sealed record DiscoveredReceiver(
    string ReceiverId,
    string DisplayName,
    string IpAddress,
    string Version,
    string FirmwareVersion,
    string InstanceNonce,
    DateTimeOffset LastSeen)
{
    public string DisplayLabel => $"{DisplayName} | IP {IpAddress} | прошивка {FirmwareVersion} | AMEGA {Version}";
}
