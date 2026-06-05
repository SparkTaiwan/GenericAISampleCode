using System;
using System.Net.Http;
using System.Text;
using System.Threading.Tasks;
using Newtonsoft.Json;

namespace SampleWrapper
{
    public class SimpleHttpClient
    {
        // Single HttpClient for the whole process. Repeated construction would leak sockets
        // into TIME_WAIT and exhaust the ephemeral port range under sustained detection load,
        // making the downstream "fail and never recover" symptom we saw in the 0524 logs.
        private static readonly HttpClient _client = CreateClient();

        private static HttpClient CreateClient()
        {
            HttpClient c = new HttpClient();
            c.Timeout = TimeSpan.FromSeconds(5);
            return c;
        }

        public async Task<string> PostAnalyticsResultAsync(string url, object data)
        {
            string jsonData = JsonConvert.SerializeObject(data);
            using (HttpContent content = new StringContent(jsonData, Encoding.UTF8, "application/json"))
            using (HttpResponseMessage response = await _client.PostAsync(url, content))
            {
                if (!response.IsSuccessStatusCode)
                {
                    string body = await response.Content.ReadAsStringAsync();
                    throw new HttpRequestException(
                        $"POST {url} returned {(int)response.StatusCode} {response.ReasonPhrase}: {body}");
                }
                return await response.Content.ReadAsStringAsync();
            }
        }
    }
}
