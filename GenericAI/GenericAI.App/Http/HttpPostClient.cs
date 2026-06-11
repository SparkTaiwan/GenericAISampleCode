using System;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Threading.Tasks;

namespace GenericAI.App
{
    // One HttpClient shared by all SendWorkers. Repeated construction would
    // leak sockets into TIME_WAIT and exhaust the ephemeral port range under
    // sustained detection load (see CSharp/SimpleHttpClient.cs:11 history).
    internal sealed class HttpPostClient
    {
        private static readonly HttpClient _client = CreateClient();

        private static HttpClient CreateClient()
        {
            HttpClient c = new HttpClient();
            c.Timeout = TimeSpan.FromSeconds(5);
            return c;
        }

        // Takes pre-serialised UTF-8 JSON so the caller can serialise once per
        // envelope and reuse the same bytes across retries; ByteArrayContent
        // also skips the extra UTF-8 copy StringContent would make.
        public async Task PostAsync(string url, byte[] jsonUtf8)
        {
            using (HttpContent content = new ByteArrayContent(jsonUtf8))
            {
                content.Headers.ContentType =
                    new MediaTypeHeaderValue("application/json") { CharSet = "utf-8" };
                using (HttpResponseMessage response = await _client.PostAsync(url, content).ConfigureAwait(false))
                {
                    if (!response.IsSuccessStatusCode)
                    {
                        string body = await response.Content.ReadAsStringAsync().ConfigureAwait(false);
                        throw new HttpRequestException(
                            $"POST {url} returned {(int)response.StatusCode} {response.ReasonPhrase}: {body}");
                    }
                }
            }
        }
    }
}
