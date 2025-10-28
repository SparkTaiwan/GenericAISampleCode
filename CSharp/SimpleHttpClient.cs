using System;
using System.Net.Http;
using System.Text;
using System.Threading.Tasks;
using Newtonsoft.Json;

namespace SampleWrapper
{
    public class SimpleHttpClient
    {
        private readonly HttpClient _client;

        public SimpleHttpClient()
        {
            _client = new HttpClient();
        }

        public async Task<string> PostAnalyticsResultAsync(string url, object data)
        {
            string jsonData = JsonConvert.SerializeObject(data);
            HttpContent content = new StringContent(jsonData, Encoding.UTF8, "application/json");
            HttpResponseMessage response = await _client.PostAsync(url, content);
            response.EnsureSuccessStatusCode();
            return await response.Content.ReadAsStringAsync();
        }
    }
}