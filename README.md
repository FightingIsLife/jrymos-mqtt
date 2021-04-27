## 基于mosquitto、mosquitto-auth-plug二次开发的一个带权限控制的广播服务

权限控制：src/be-redis.c


## mosquitto调优介绍：

### 调优方向

调优目标：
1. 尽可能支撑更多的订阅者数量
2. 1s内尽可能的推送更多的消息数量

底线：订阅消息丢失率小于1% (不考虑客户端网络环境)

定义调优指标：
定义Q = 每秒客户端接收消息总条数 （Q也可以认为就是吞吐量）
例如：
```
1w订阅者，每秒最多能推230条消息，Q = 1w * 230 = 230w  记作Q(1w)
2w订阅者，每秒最多能推130条消息，Q = 2w * 130 = 260w  记作Q(2w)
5w订阅者，每秒最多能推50条消息，Q = 5w * 50 = 250w 记作Q(5w)
10w订阅者，每秒最多能推22条消息，Q = 10w * 22 = 220w 记作Q(10w)
```
Q值越大说明调优效果越好

### 准备压测，压测主要代码介绍：
```
//一个mqtt服务，相关的连接
private static class SingleServer {
    private Publisher publisher; //一个发布者
    private List<Subscriber> subscribers; //n个订阅者
    private int sendNumber = Interger.MAX_VALUE; //发送消息条数
    private String topic = "jrymos:test"; //定义好一个特殊的topic，进行压测
    private String message; //发送消息内容
    private boolean compress; //true开启压缩字符串
    private int combiningNumber = 1;//合并写条数
    private int qos = 2;//发布的消息服务质量，0 最多一次，1只保证1次，2至少1次

    private Statistics statistics; //统计信息,用来统计压测结果

    //启动订阅者
    public void startSubscriber(int subscriberNumber) {
        subscribers = new ArrayList<>(subscriberNumber);
        for (int j = 0; j < subscriberNumber; j++) {
            subscribers.add(new StatisticsSubscriber(statistics, topic));
        }
    }

    //开始推送
    public void push() {
        try {
            //合并消息
            String combiningMessage = message;
            for (int i = 1; i < combiningNumber; i++) {
                combiningMessage = combiningMessage + "," + message;
            }
            //推送消息
            for (int i = 0; i < sendNumber; i = i + combiningNumber) {
                byte[] bytes = compress ? Snappy.compress(combiningMessage) : combiningMessage.getBytes("UTF-8");
                publisher.publish(topic, combiningMessage);
            }
            //等待全部推送完成
            boolean allSuccess;
            do {
                Thread.sleep(1000);
                Statistics clone = statistics.clone();
                allSuccess = clone.isAllSuccessSubscribe();
                //打印推送过程信息
                System.out.println(clone);
            } while (!allSuccess);
        } catch (UnsupportedEncodingException | MqttException | InterruptedException | CloneNotSupportedException e) {
            e.printStackTrace();
        }
    }
}

//统计信息
public class Statistics implements Cloneable {
    private LongAdder publishCount = new LongAdder(); //记录已经推送条数
    private LongAdder publishSuccessCount = new LongAdder(); //记录已经推送成功条数
    //记录订阅成功条数，如果没有丢失任何消息：subscribeSuccessCount=subscriberNumber * publishSuccessCount
    private LongAdder subscribeSuccessCount = new LongAdder(); 
    private final int subscriberNumber; //订阅者数量
    private final StopWatch stopWatch; //记录时间
}
```


### 单机单实例连接数压测
单机单进程，端口tcp连接数最大4096，首次将订阅数设置500，观察推送统计数据，然后逐渐调高订阅数至4k

```
订阅数  Q值
500     1.6w
1000    1.7w
1500    1.6w
2000    1.5w
3000    1.3w
4000    1.1w
```

### mqtt配置、代码优化
persistence false 内存持久化到磁盘设置为false
qos 设置为0
be-redis.c中的acl校验topic时不访问redis

```
订阅数  Q值
500     4w
1000    4w
1500    3.9w
2000    3.7w
3000    3.2w
4000    3w
```

**经过压测后，单端口订阅数适合在1k~2k之间**

### 单机多实例优化

在同一个机器上创建多个mqtt实例，实例个数从2调到20

```
mqtt    订阅数  Q值
1个      2k    3.7w
2个      4k    7.2w
5个      10k   16.8w
10个     20k   30w
15个     30k   27.5w
20个     40k   21w

10个     10k   31.5w
20个     20k   22w
```

**经过压测后，单机mqtt实例个数适合在10~15之间，最终选择了15个，因为他支持更多的订阅数**


### 业务方使用优化
##### 合并推送，将多条消息合并成一条消息推送给客户端，减少推送次数

```
合并推送    订阅数     Q值
不合并      30k       27.5w
5条         30k       58w
10条        30k       60w
20条        30k       59w
```

##### 字符串压缩（结合合并推送）

```
不压缩：
合并推送    订阅数     Q值
不合并      30k       27.5w
5条         30k       58w
10条        30k       60w
20条        30k       59w

压缩：
合并推送    订阅数     Q值
不合并      30k       35.5w
5条         30k       78w
10条        30k       99w
20条        30k       105w
```

[java业务方的使用](https://github.com/FightingIsLife/jrymos-broadcast)


### 多机水平扩展
开了4台机器，每台都部署15个mqtt实例共60个mqtt实例，每个mqtt实例支持2k个订阅者，最终一共支持12w的连接数。
每台吞吐量100w/s，最高吞吐量400w/s


##连接优化（主要优化的是提升连接数，和吞吐量关系不大）

##### mqtt心跳配置
`keepalive_interval 60`

##### tcp内存配置（根据需要调整最大内存，以支持更多的连接数）

```
# 内核分给TCP的内存大小范围，单位为page
net.ipv4.tcp_mem = 491520 786432 983040
```

##### tcp连接优化 
```
# 保持在FIN-WAIT-2状态的时间 30s
net.ipv4.tcp_fin_timeout = 30
# TCP发送keepalive消息的频度 600s
net.ipv4.tcp_keepalive_time = 600
```

##### 思考：这几个参数是否应该设置呢？
```
# 允许将TIME-WAIT sockets重新用于新的TCP连接
net.ipv4.tcp_tw_reuse=1
# 开启TCP连接中TIME-WAIT sockets的快速回收
net.ipv4.tcp_tw_recycle=1
# 单个TCP连接读缓存大小
net.ipv4.tcp_rmem = 4096 4096 65536
# 单个TCP连接写缓存大小
net.ipv4.tcp_wmem = 4096 8192 65536
```