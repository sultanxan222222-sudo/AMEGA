using System.Text.Json.Serialization;

namespace AMEGA.Windows.Protocol;

internal sealed record TransferOffer(
    [property: JsonPropertyName("protocol")] string Protocol,
    [property: JsonPropertyName("major")] int Major,
    [property: JsonPropertyName("type")] string Type,
    [property: JsonPropertyName("transfer_id")] string TransferId,
    [property: JsonPropertyName("sender_id")] string SenderId,
    [property: JsonPropertyName("file_name")] string FileName,
    [property: JsonPropertyName("file_size")] long FileSize,
    [property: JsonPropertyName("file_kind")] string FileKind,
    [property: JsonPropertyName("sha256")] string Sha256,
    [property: JsonPropertyName("created_utc")] DateTimeOffset CreatedUtc);
