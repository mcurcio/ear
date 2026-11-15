// A real report feed, captured from shairport-sync 5.6.3 (docker/Dockerfile's build) run as the airplay node runs it
// (--name Kitchen --port 5100, its config as configText writes it) while pyatv played a 3 s tone to it as AirPlay 1
// (test/support/airplay_sender.py): its listing twice (the second with a busy flag in sf), a session's items between
// pbeg and pend, and its other reports, which the node ignores. The parser's every-boundary test splits it everywhere.
#pragma once
#include <string_view>
namespace airplay_test {
inline constexpr std::string_view capturedFeed =
	R"feed(<item><type>73736e63</type><code>73766e61</code><length>7</length>
<data encoding="base64">
S2l0Y2hlbg==</data></item>
<item><type>73736e63</type><code>73767267</code><length>271</length>
<data encoding="base64">
c2VydmljZT1fcmFvcC5fdGNwCm5hbWU9RDYwNjU2MjVBRjI5QEtpdGNoZW4KcG9ydD01MTAwCnR4dD1zZj0weDQKdHh0PWZ2PTdjZTkwZDgKdHh0PWFtPVNoYWlycG9ydFN5bmMKdHh0PXZzPTEwNS4xCnR4dD10cD1UQ1AsVURQCnR4dD12bj02NTUzNwp0eHQ9bWQ9MCwxLDIKdHh0PXNzPTE2CnR4dD1zcj00NDEwMAp0eHQ9ZGE9dHJ1ZQp0eHQ9c3Y9ZmFsc2UKdHh0PWV0PTAsMQp0eHQ9ZWs9MQp0eHQ9Y249MCwxCnR4dD1jaD0yCnR4dD10eHR2ZXJzPTEKdHh0PXB3PWZhbHNlCg==</data></item>
<item><type>73736e63</type><code>73767267</code><length>275</length>
<data encoding="base64">
c2VydmljZT1fcmFvcC5fdGNwCm5hbWU9RDYwNjU2MjVBRjI5QEtpdGNoZW4KcG9ydD01MTAwCnR4dD1zZj0weDIwMDA0CnR4dD1mdj03Y2U5MGQ4CnR4dD1hbT1TaGFpcnBvcnRTeW5jCnR4dD12cz0xMDUuMQp0eHQ9dHA9VENQLFVEUAp0eHQ9dm49NjU1MzcKdHh0PW1kPTAsMSwyCnR4dD1zcz0xNgp0eHQ9c3I9NDQxMDAKdHh0PWRhPXRydWUKdHh0PXN2PWZhbHNlCnR4dD1ldD0wLDEKdHh0PWVrPTEKdHh0PWNuPTAsMQp0eHQ9Y2g9Mgp0eHQ9dHh0dmVycz0xCnR4dD1wdz1mYWxzZQo=</data></item>
<item><type>73736e63</type><code>636f6e6e</code><length>9</length>
<data encoding="base64">
MTI3LjAuMC4x</data></item>
<item><type>73736e63</type><code>636c6970</code><length>9</length>
<data encoding="base64">
MTI3LjAuMC4x</data></item>
<item><type>73736e63</type><code>73766970</code><length>9</length>
<data encoding="base64">
MTI3LjAuMC4x</data></item>
<item><type>73736e63</type><code>736e7561</code><length>14</length>
<data encoding="base64">
QWlyUGxheS81NTAuMTA=</data></item>
<item><type>73736e63</type><code>61637265</code><length>9</length>
<data encoding="base64">
MjkzMDY2MjE0</data></item>
<item><type>73736e63</type><code>64616964</code><length>16</length>
<data encoding="base64">
REQ3OERCQTQ5MkI5QkJBRA==</data></item>
<item><type>73736e63</type><code>636c6970</code><length>9</length>
<data encoding="base64">
MTI3LjAuMC4x</data></item>
<item><type>73736e63</type><code>73766970</code><length>9</length>
<data encoding="base64">
MTI3LjAuMC4x</data></item>
<item><type>73736e63</type><code>70766f6c</code><length>25</length>
<data encoding="base64">
LTIwLjEwLC00My42MiwtOTYuMzAsMC4wMA==</data></item>
<item><type>73736e63</type><code>70726772</code><length>18</length>
<data encoding="base64">
NjYxNTAvNjYxNTAvMTk4NDUw</data></item>
<item><type>73736e63</type><code>6d647374</code><length>5</length>
<data encoding="base64">
NjYxNTA=</data></item>
<item><type>73736e63</type><code>6d64656e</code><length>5</length>
<data encoding="base64">
NjYxNTA=</data></item>
<item><type>73736e63</type><code>61626567</code><length>0</length></item>
<item><type>73736e63</type><code>70626567</code><length>0</length></item>
<item><type>73736e63</type><code>70766f6c</code><length>25</length>
<data encoding="base64">
LTIwLjEwLC00My42MiwtOTYuMzAsMC4wMA==</data></item>
<item><type>73736e63</type><code>73647363</code><length>19</length>
<data encoding="base64">
QUxBQy80NDEwMC9TMTZfTEUvMg==</data></item>
<item><type>73736e63</type><code>7072736d</code><length>0</length></item>
<item><type>73736e63</type><code>70666672</code><length>17</length>
<data encoding="base64">
NjYxNTAvMjQwMTM3OTgxNjA=</data></item>
<item><type>73736e63</type><code>73747970</code><length>7</length>
<data encoding="base64">
QWlyUGxheQ==</data></item>
<item><type>73736e63</type><code>73647363</code><length>19</length>
<data encoding="base64">
QUxBQy80NDEwMC9TMTZfTEUvMg==</data></item>
<item><type>73736e63</type><code>6f647363</code><length>14</length>
<data encoding="base64">
NDQxMDAvUzE2X0xFLzI=</data></item>
<item><type>73736e63</type><code>70656e64</code><length>0</length></item>
<item><type>73736e63</type><code>73767267</code><length>271</length>
<data encoding="base64">
c2VydmljZT1fcmFvcC5fdGNwCm5hbWU9RDYwNjU2MjVBRjI5QEtpdGNoZW4KcG9ydD01MTAwCnR4dD1zZj0weDQKdHh0PWZ2PTdjZTkwZDgKdHh0PWFtPVNoYWlycG9ydFN5bmMKdHh0PXZzPTEwNS4xCnR4dD10cD1UQ1AsVURQCnR4dD12bj02NTUzNwp0eHQ9bWQ9MCwxLDIKdHh0PXNzPTE2CnR4dD1zcj00NDEwMAp0eHQ9ZGE9dHJ1ZQp0eHQ9c3Y9ZmFsc2UKdHh0PWV0PTAsMQp0eHQ9ZWs9MQp0eHQ9Y249MCwxCnR4dD1jaD0yCnR4dD10eHR2ZXJzPTEKdHh0PXB3PWZhbHNlCg==</data></item>
<item><type>73736e63</type><code>61656e64</code><length>0</length></item>
)feed";
} // namespace airplay_test
