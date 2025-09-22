/**
 * WebSocket Subscription Client Example
 *
 * This example demonstrates how to connect to InstantDB and create
 * WASM-powered subscription queries via WebSocket.
 */

class InstantDBClient {
    constructor(url) {
        this.url = url;
        this.ws = null;
        this.clientId = null;
        this.subscriptions = new Map();
        this.messageId = 0;
        this.pendingRequests = new Map();
    }

    async connect() {
        return new Promise((resolve, reject) => {
            this.ws = new WebSocket(this.url);

            this.ws.onopen = () => {
                console.log('Connected to InstantDB');
            };

            this.ws.onmessage = (event) => {
                this.handleMessage(JSON.parse(event.data));
            };

            this.ws.onclose = () => {
                console.log('Disconnected from InstantDB');
            };

            this.ws.onerror = (error) => {
                console.error('WebSocket error:', error);
                reject(error);
            };

            // Wait for welcome message
            this.pendingRequests.set('welcome', { resolve, reject });
        });
    }

    handleMessage(message) {
        console.log('Received:', message);

        switch (message.type) {
            case 'welcome':
                this.clientId = message.client_id;
                console.log(`Welcome! Client ID: ${this.clientId}`);
                console.log(`Features: ${message.features.join(', ')}`);

                const welcomeHandler = this.pendingRequests.get('welcome');
                if (welcomeHandler) {
                    welcomeHandler.resolve(message);
                    this.pendingRequests.delete('welcome');
                }
                break;

            case 'wasm_subscription_created':
                console.log(`WASM subscription created: ${message.subscription_id}`);
                this.subscriptions.set(message.subscription_id, {
                    id: message.subscription_id,
                    module_name: message.module_name,
                    client_id: message.client_id
                });

                const createHandler = this.pendingRequests.get(message.subscription_id);
                if (createHandler) {
                    createHandler.resolve(message);
                    this.pendingRequests.delete(message.subscription_id);
                }
                break;

            case 'wasm_subscription_event':
                this.handleSubscriptionEvent(message);
                break;

            case 'changefeed_event':
                this.handleChangefeedEvent(message);
                break;

            case 'error':
                console.error('Server error:', message.message);
                break;

            case 'pong':
                console.log('Pong received');
                break;

            default:
                console.log('Unknown message type:', message.type);
        }
    }

    handleSubscriptionEvent(message) {
        console.log('📨 WASM Subscription Event:');
        console.log(`  Subscription: ${message.subscription_id}`);
        console.log(`  Client: ${message.client_id}`);
        console.log(`  Data:`, message.data);

        // You can add custom handlers here based on subscription ID
        const subscription = this.subscriptions.get(message.subscription_id);
        if (subscription && subscription.onEvent) {
            subscription.onEvent(message.data);
        }
    }

    handleChangefeedEvent(message) {
        console.log('📊 Changefeed Event:');
        console.log(`  Table: ${message.table}`);
        console.log(`  Operation: ${message.operation}`);
        console.log(`  Key: ${message.key}`);
        if (message.new_value) {
            console.log(`  New Value: ${message.new_value}`);
        }
    }

    async createWasmSubscription(options) {
        const subscriptionId = `sub_${++this.messageId}`;

        const request = {
            type: 'wasm_subscribe',
            module_name: options.moduleName,
            ...options
        };

        return new Promise((resolve, reject) => {
            this.pendingRequests.set(subscriptionId, { resolve, reject });

            // Store expected subscription ID for response matching
            this.pendingRequests.set(subscriptionId + '_create', { resolve, reject });

            this.ws.send(JSON.stringify(request));

            // Timeout after 10 seconds
            setTimeout(() => {
                if (this.pendingRequests.has(subscriptionId)) {
                    this.pendingRequests.delete(subscriptionId);
                    reject(new Error('Subscription creation timeout'));
                }
            }, 10000);
        });
    }

    async subscribeToRecentActivities(onEvent) {
        return this.createWasmSubscription({
            moduleName: 'subscription_demo',
            filter_function: 'filter_recent_activities',
            transform_function: 'transform_add_metadata',
            tables: ['user_activities'],
            include_initial_data: true,
            onEvent
        });
    }

    async subscribeToPremiumUsers(onEvent) {
        return this.createWasmSubscription({
            moduleName: 'subscription_demo',
            filter_function: 'filter_premium_users',
            transform_function: 'transform_anonymize',
            tables: ['user_activities'],
            where_clause: 'premium = true',
            onEvent
        });
    }

    ping() {
        this.ws.send(JSON.stringify({ type: 'ping' }));
    }

    disconnect() {
        if (this.ws) {
            this.ws.close();
        }
    }
}

// Example usage
async function main() {
    const client = new InstantDBClient('ws://localhost:8080');

    try {
        // Connect to the server
        await client.connect();

        // Subscribe to recent activities with metadata
        console.log('\n🔔 Creating subscription to recent activities...');
        await client.subscribeToRecentActivities((data) => {
            console.log('📈 Recent Activity Update:', data);
        });

        // Subscribe to premium user activities (anonymized)
        console.log('\n🔔 Creating subscription to premium user activities...');
        await client.subscribeToPremiumUsers((data) => {
            console.log('👑 Premium User Activity (Anonymized):', data);
        });

        // Send periodic pings
        setInterval(() => {
            client.ping();
        }, 30000);

        console.log('\n✅ Subscriptions created! Listening for events...');
        console.log('💡 Tip: Use gRPC client to create activities and see real-time updates!');

    } catch (error) {
        console.error('Failed to set up subscriptions:', error);
    }
}

// Run the example if this is the main module
if (typeof window === 'undefined') {
    // Node.js environment
    const WebSocket = require('ws');
    main().catch(console.error);
} else {
    // Browser environment
    window.InstantDBClient = InstantDBClient;
    window.runExample = main;
    console.log('InstantDBClient loaded. Call runExample() to start.');
}

/**
 * Example subscription configurations:
 *
 * 1. Real-time user activities (last hour only):
 * {
 *   moduleName: 'subscription_demo',
 *   filter_function: 'filter_recent_activities',
 *   transform_function: 'transform_add_metadata',
 *   tables: ['user_activities'],
 *   include_initial_data: true
 * }
 *
 * 2. Premium user activities (anonymized):
 * {
 *   moduleName: 'subscription_demo',
 *   filter_function: 'filter_premium_users',
 *   transform_function: 'transform_anonymize',
 *   tables: ['user_activities'],
 *   where_clause: 'premium = true'
 * }
 *
 * 3. All activities with custom parameters:
 * {
 *   moduleName: 'subscription_demo',
 *   tables: ['user_activities'],
 *   columns: ['user_id', 'action', 'timestamp'],
 *   parameters: {
 *     min_reputation: 50,
 *     include_anonymous: false
 *   },
 *   start_offset: 0,
 *   include_initial_data: false
 * }
 */