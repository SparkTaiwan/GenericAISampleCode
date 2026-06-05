using System;
using System.Net.Http;
using System.Text;
using System.Threading.Tasks;
using Newtonsoft.Json;

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

        public async Task PostAsync(string url, object payload)
        {
            string json = JsonConvert.SerializeObject(payload);
            using (HttpContent content = new StringContent(json, Encoding.UTF8, "application/json"))
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
