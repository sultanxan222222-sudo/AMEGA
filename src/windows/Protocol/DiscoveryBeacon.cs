using System.Text.Json.Serialization;

namespace AMEGA.Windows.Protocol;

internal sealed record DiscoveryBeacon(
    [property: JsonPropertyName("protocol")] string Protocol,
    [property: JsonPropertyName("major")] int Major,
    [property: JsonPropertyName("type")] string Type,
    [property: JsonPropertyName("sender_id")] string SenderId,
    [property: JsonPropertyName("instance_nonce")] string InstanceNonce,
    [property: JsonPropertyName("http_port")] int HttpPort,
    [property: JsonPropertyName("display_name")] string DisplayName);
