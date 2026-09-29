# Repeater health and link history

## Repeater health

Open **Tools > Admin**, choose a saved repeater or room, and log in. The first
tab, **Health**, requests the remote status packet and displays battery voltage,
uptime, outbound queue length, and noise floor / last RSSI. It then requests the
remote firmware version, board name, and recent neighbor list. Select **Nbrs**
to read the neighbor reply, or **Refresh** to update the snapshot.

Status replies use the fixed header shared by repeater and room firmware. A
timeout or unsupported field is shown on screen. Neighbor counts are based on
the text returned by the remote `neighbors` command, which contains at most
eight recent adverts; they are not a complete topology count. The version,
board, and neighbor requests run one at a time so the radio is not flooded.

## Link history

Open **Tools > Nodes**, run **Discover scan**, and choose **Link history** from
a node's options menu. From its detail view, **Enter** is a shortcut. Each completed scan contributes one RSSI
and SNR sample per responding node. The view shows the latest reading, average
RSSI, first-to-last RSSI change, and an eight-scan RSSI graph.

The history is held in RAM for up to 12 nodes and eight scans per node. It
survives leaving Nodes but resets when the device restarts. It samples only
direct discovery replies, so a node absent from a scan receives no new sample.
