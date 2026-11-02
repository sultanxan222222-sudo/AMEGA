using System.Text.Json.Serialization;

namespace AMEGA.Windows.Networking;

internal sealed record AcceptTransferRequest(
    [property: JsonPropertyName("transfer_id")] string TransferId,
    [property: JsonPropertyName("receiver_id")] string ReceiverId,
    [property: JsonPropertyName("instance_nonce")] string InstanceNonce,
    [property: JsonPropertyName("reply_to_nonce")] string ReplyToNonce);
