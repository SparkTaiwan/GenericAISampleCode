namespace GenericAI.App
{
    // Single source of truth for the recorder<->wrapper wire protocol version.
    // Emitted on GET /Alive (so the recorder's AI Service Module can learn the
    // wrapper's protocol level before it POSTs /SetParameters) and stamped on
    // every result envelope (HttpEnvelope.version / ZMQ result). The recorder
    // currently accepts "1.2" and "1.3"; bump this in lockstep with the
    // "version" the recorder sends in /SetParameters
    // (spark.recorder/modules/AIService/Generic/AStreamPerimeter_GenericAI.cpp).
    internal static class Protocol
    {
        public const string Version = "1.3";
    }
}
